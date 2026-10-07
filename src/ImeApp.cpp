#include "ImeApp.h"

#include "WCharUtils.h"
#include "common.h"
#include "configs/ConfigSerializer.h"
#include "configs/CustomMessage.h"
#include "configs/configuration.h"
#include "configs/settings_converter.h"
#include "core/EventHandler.h"
#include "core/State.h"
#include "hook.h"
#include "hooks/MeridianBridge.h"
#include "hooks/NirnLabBridge.h"
#include "hooks/PrismaBridge.h"
#include "misc/freetype/imgui_freetype.h"
#include "hooks/ScaleformHook.h"
#include "hooks/SkseMenuFrameworkBridge.h"
#include "hooks/WinHooks.h"
#include "ime/ImeController.h"
#include "imguiex/ErrorNotifier.h"
#include "imguiex/imgui_manager.h"
#include "imguiex/imguiex_m3.h"
#include "log.h"
#include "menu/ImeMenu.h"
#include "menu/ToolWindowMenu.h"
#include "path_utils.h"
#include "ui/Settings.h"
#include "ui/SettingsManager.h"
#include "ui/fonts/FontManager.h"

#include <basetsd.h>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace
{
class InitErrorMessageShow final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
    std::queue<std::string> m_message;
    bool                    m_installedSink = false;

public:
    InitErrorMessageShow()
    {
        if (auto *ui = RE::UI::GetSingleton(); ui != nullptr)
        {
            ui->AddEventSink(this);
            m_installedSink = true;
        }
    }

    ~InitErrorMessageShow() override
    {
        if (auto *ui = RE::UI::GetSingleton(); m_installedSink && ui != nullptr)
        {
            ui->RemoveEventSink(this);
        }
    }

    auto ProcessEvent(const RE::MenuOpenCloseEvent *a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent> * /*a_eventSource*/)
        -> RE::BSEventNotifyControl override
    {
        if (a_event->menuName == RE::MainMenu::MENU_NAME && a_event->opening)
        {
            while (!m_message.empty())
            {
                RE::DebugMessageBox(m_message.front().c_str());
                m_message.pop();
            }
            RE::UI::GetSingleton()->RemoveEventSink(this);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    void PushMessage(std::string &&message)
    {
        if (m_installedSink)
        {
            m_message.emplace(std::move(message));
        }
    }
};

#ifdef _DEBUG
constexpr auto INIT_TIMEOUT_SECONDS = 500s;
#else
constexpr auto INIT_TIMEOUT_SECONDS = 5s;
#endif
std::unique_ptr<Ime::ImeApp>            g_instance = nullptr;
std::unique_ptr<InitErrorMessageShow>   g_pInitErrorMessageShow(nullptr);
std::unique_ptr<Hooks::D3DInitHookData> g_D3DInitHook = nullptr; ///< Only install once, should not be a member of `ImeApp`.
std::unique_ptr<Hooks::D3DPresentHookData> g_PresentHook = nullptr;

// The real IDXGISwapChain::Present detour (see ImeApp::SwapChainPresentHook).
// The FunctionHook aliases g_realSwapChainPresent, which must outlive it.
Hooks::FunctionHook<long(void *, std::uint32_t, std::uint32_t)> *g_swapChainPresentHook     = nullptr;
void                   *g_realSwapChainPresent       = nullptr;
ID3D11DeviceContext    *g_swapChainContext           = nullptr;
ID3D11RenderTargetView *g_swapChainBackBufferRtv     = nullptr;

// SimpleIME's per-frame work (ImGui rendering, MeridianBridge::Tick) is driven
// from ImeMenu::PostDisplay — but the engine STOPS calling it while a Meridian
// view holds focus (Meridian renders through its own CEF path; observed in the
// 2026-10-01 debug log: composition events fired, yet zero ImGui frames, no
// panel and no bridge tick for the whole session). The swapchain present still
// fires every frame in that state, so it acts as the fallback frame driver.
// The two drivers exclude each other; the frame token makes the present hook
// stand down while the normal PostDisplay path is alive. The token therefore
// measures ONLY PostDisplay liveness and is written exclusively by ImeApp::Draw
// — the present hook must never refresh it, or it would mark itself alive,
// stand down for PRESENT_FRAME_TAKEOVER_MS, and degrade the fallback to ~4 fps.
std::mutex                 g_ImGuiFrameMutex;
std::atomic<std::uint64_t> g_lastUiFrameMs{0};
constexpr auto             PRESENT_FRAME_TAKEOVER_MS = 250;
} // namespace

namespace SksePlugin
{
auto Initialize() -> bool
{
    g_instance    = std::make_unique<Ime::ImeApp>();
    g_D3DInitHook = std::make_unique<Hooks::D3DInitHookData>(Ime::D3DInit);
    Hooks::WinHooks::Install();

    auto &errorNotifier = ErrorNotifier::GetInstance();
    errorNotifier.SetMessageDuration(g_instance->GetSettings().appearance.errorDisplayDuration);
#ifdef _DEBUG
    errorNotifier.SetMessageLevel(ErrorMsg::Level::debug);
#endif

    const auto *plugin  = SKSE::PluginDeclaration::GetSingleton();
    const auto  version = plugin->GetVersion();
    logger::info("{}({}) has finished loading.", plugin->GetName(), version.string("."));

    InitializeMessaging();
    return true;
}

void InitializeMessaging()
{
    using State = Ime::Core::State;
    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message *a_msg) -> void {
        if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame)
        {
            State::GetInstance().Set(State::GAME_LOADING);
            // The menu stack (and any SKSEMF text field with it) is torn down
            // underneath us; no ImGui frame will ever report the field's
            // deactivation, so end the session deterministically.
            Hooks::SkseMenuFrameworkBridge::ForceEndSession();
        }
        else if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame)
        {
            State::GetInstance().Clear(State::GAME_LOADING);
        }
        else if (a_msg->type == SKSE::MessagingInterface::kInputLoaded)
        {
            Ime::ImeApp::GetInstance().OnInputLoaded();
            // UIPlatform protocol (version accepted at kPostPostLoad): request
            // the API pointer. The synchronous response hooks the public
            // vtable, so every UIPlatform browser created afterwards is
            // observed.
            Hooks::NirnLabBridge::RequestApi();
        }
        else if (a_msg->type == SKSE::MessagingInterface::kPostPostLoad)
        {
            // Every SKSE plugin DLL is loaded by now: negotiate Prisma's public
            // focus interface for the avoidance mode (read-only).
            Hooks::PrismaBridge::Install();
            // NirnLabUIPlatform (the UIPlatform flavor of Meridian) is
            // negotiated over SKSE messaging, not exports: ask for its
            // protocol version now, its API pointer at kInputLoaded. The
            // response installs the UIPlatform focus backend, the primary
            // Meridian focus source.
            Hooks::NirnLabBridge::InstallMessaging();
        }
        else if (a_msg->type == SKSE::MessagingInterface::kDataLoaded)
        {
            // MeridianUI.dll is a SKSE plugin, so it is loaded by now too;
            // negotiate Meridian.View/1 and observe its focus. This is the
            // fallback backend — UIPlatform browsers never show up here.
            Hooks::MeridianBridge::Install();
            // Same for SKSEMenuFramework.dll: detect its exports and register
            // the render-event bridge for its ImGui text fields.
            Hooks::SkseMenuFrameworkBridge::Install();
        }
    });
}
} // namespace SksePlugin

namespace Ime
{
namespace
{
//! Appends system fonts for the scripts a primary font often lacks — Hangul
//! (한국어) and Kana (日本語). ImGui 1.92 rasterizes merged-font glyphs on
//! demand, so a family costs nothing unless one of its glyphs is drawn.
void AppendCjkSupplementaryFonts(std::vector<std::string> &fonts)
{
    const auto appendFamily = [&fonts](const std::wstring_view family) -> bool {
        if (const auto fontFilePath = GetFirstFontFilePathInFamily(family); !fontFilePath.empty())
        {
            fonts.push_back(WCharUtils::ToString(fontFilePath));
            logger::info("Supplementary CJK font: {}", fonts.back());
            return true;
        }
        return false;
    };

    appendFamily(Settings::DEFAULT_KOREAN_FONT_FAMILY);
    // Yu Gothic is the modern Japanese family (Win 8.1+); MS Gothic ships on
    // every Windows and covers stripped installs that dropped the Yu families.
    if (!appendFamily(Settings::DEFAULT_JAPANESE_FONT_FAMILY))
    {
        appendFamily(Settings::DEFAULT_JAPANESE_FALLBACK_FAMILY);
    }
}

auto GetDefaultFontFilePathList() -> std::vector<std::string>
{
    std::vector<std::string> fonts{};

    const auto primaryFontFilePath = GetDefaultFontFilePath();
    if (primaryFontFilePath.empty())
    {
        ErrorNotifier::GetInstance().Warning("Can't get default font. Fallback to ImGui embedded font.");
        return std::vector<std::string>{};
    }
    fonts.push_back(WCharUtils::ToString(primaryFontFilePath));
    logger::info("Primary font: {}", fonts.back());

    std::wstring emojiFontFilePath = GetFirstFontFilePathInFamily(Settings::DEFAULT_EMOJI_FONT_FAMILY);
    if (emojiFontFilePath.empty())
    {
        emojiFontFilePath = GetFirstFontFilePathInFamily(Settings::DEFAULT_SYMBOL_FONT_FAMILY);
    }
    if (!emojiFontFilePath.empty())
    {
        fonts.push_back(WCharUtils::ToString(emojiFontFilePath));
        logger::info("Supplementary font: {}", fonts.back());
    }
    AppendCjkSupplementaryFonts(fonts);
    return fonts;
}
} // namespace

ImeApp::ImeApp()
{
    m_settings = SettingsManager::Load();

    // Seed the conversion-mode (中/英) prior from the previous session's last
    // in-game observation (persisted as input.last_native_conversion). This
    // marks the mode as observed, so the activation path's "unknown mode ⇒
    // assume 中" seed never fires on top of it — the remembered value is the
    // better first-entry prior. A config without the key (fresh install or
    // pre-existing file) loads the default true, i.e. the same 中 guess the
    // activation seed would have made.
    auto &conversionState = Core::State::GetInstance();
    if (m_settings.input.lastNativeConversion)
    {
        conversionState.AddConversionModeFlag(Core::State::ConversionMode::Flags::NATIVE);
    }
    else
    {
        conversionState.ClearConversionModeFlag(Core::State::ConversionMode::Flags::NATIVE);
    }

    SksePlugin::InitializeLogging({.level = m_settings.logging.level, .flushLevel = m_settings.logging.flushLevel});
}

auto ImeApp::GetInstance() -> ImeApp &
{
    return *g_instance;
}

void ImeApp::OnInputLoaded()
{
    if (m_state.IsInitialized())
    {
        Events::InstallEventSinks();
    }
}

void ImeApp::Uninitialize()
{
    // One-shot: both Shutdown() and the WM_NCDESTROY path route here; a second
    // entry must not repeat the ImGui/TSF teardown below.
    if (m_uninitializeStarted.test_and_set(std::memory_order_acq_rel))
    {
        return;
    }
    // The WM_NCDESTROY path reaches Uninitialize without passing Shutdown, so
    // the worker's quit request (and the bounded wait) belongs here, not only
    // in Shutdown().
    RequestImeThreadTeardown();
    SaveSettings();
    if (m_imeTeardownDone.load())
    {
        // IME worker confirmed finished — its teardown (which releases
        // m_imeOverlay, an ImGui user) ran to completion before this point,
        // so destroying ImGui here is properly ordered after it.
        ImGuiEx::M3::Destroy();
        ImGuiEx::Shutdown();
    }
    else
    {
        // Shutdown() timed out: the IME worker may still wake up later and run
        // its own teardown (m_imeOverlay reset, controller shutdown, TSF
        // uninit) on the IME thread. Skipping the ImGui teardown keeps the
        // context alive so that late teardown stays valid; at process exit the
        // leaked context is reclaimed by the OS. Concurrently destroying ImGui
        // here would be the race.
        logger::warn("IME thread teardown not confirmed; skipping ImGui destruction to avoid racing it.");
    }
    Events::UnInstallEventSinks(); // should safety
    Hooks::WinHooks::Uninstall();  // should move to dll_detach, but it's safe.
    Hooks::MeridianBridge::Uninstall();
    Hooks::PrismaBridge::Uninstall();
    Hooks::SkseMenuFrameworkBridge::Uninstall();
    g_pInitErrorMessageShow.reset();
    if (m_state.IsInitialized())
    {
        UninstallHooks();
        if (RealWndProc != nullptr)
        {
            SetWindowLongPtrA(m_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RealWndProc));
            RealWndProc = nullptr;
        }
    }
    m_state.SetState(State::StateKey::DORMANCY);
}

void D3DInit()
{
    auto &app = ImeApp::GetInstance();
    // This hook REPLACES the original call site, so the engine's own D3D
    // initialization must run on EVERY invocation — including the "already
    // initialized" bail-out below (a renderer rebuild after a device reset
    // would otherwise have its D3D init silently skipped and black-screen).
    g_D3DInitHook->Original();
    if (!app.m_state.IsUnInitialized())
    {
        logger::warn("Already Initialized! Current state: {}", app.m_state.GetStateKetText());
        return;
    }
    g_pInitErrorMessageShow = std::make_unique<InitErrorMessageShow>();
    try
    {
        app.DoD3DInit();
        g_pInitErrorMessageShow.reset();
        return;
    }
    catch (std::exception &error)
    {
        app.m_state.SetState(ImeApp::State::StateKey::INITIALIZE_FAILED);
        auto message = std::format("SimpleIME initialize fail: \n {}", error.what());
        logger::error(message.c_str());
        g_pInitErrorMessageShow->PushMessage(std::move(message));
    }
    catch (...)
    {
        app.m_state.SetState(ImeApp::State::StateKey::INITIALIZE_FAILED);
        auto message = std::string("SimpleIME: Unknown fatal error during D3DInit.");
        logger::error(message.c_str());
        g_pInitErrorMessageShow->PushMessage(std::move(message));
    }

    app.Shutdown();
}

void ImeApp::DoD3DInit()
{
    // The engine's Original() already ran in the D3DInit hook replacement.
    m_state.SetState(State::StateKey::INITIALIZING);
    OnD3DInit();
}

void ImeApp::OnD3DInit()
{
    auto *renderManager = RE::BSGraphics::Renderer::GetSingleton();
    if (renderManager == nullptr)
    {
        throw SimpleIMEException("Cannot find render manager. Initialization failed!");
    }

    const auto &renderData = renderManager->GetRuntimeData();
    logger::debug("Getting SwapChain...");
    auto *pSwapChain = renderData.renderWindows->swapChain;
    if (pSwapChain == nullptr)
    {
        throw SimpleIMEException("Cannot find SwapChain. Initialization failed!");
    }

    logger::debug("Getting SwapChain desc...");
    REX::W32::DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    if (pSwapChain->GetDesc(&swapChainDesc) < 0)
    {
        throw SimpleIMEException("IDXGISwapChain::GetDesc failed.");
    }

    m_hWnd = reinterpret_cast<HWND>(swapChainDesc.outputWindow);

    // Swapchain-level present hook (see ImeApp::SwapChainPresentHook): PrismaUI
    // draws its Ultralight views from a present CALL-SITE hook that always runs
    // after our PostDisplay overlay; the real swapchain present is the only
    // point guaranteed to be after every such draw.
    g_swapChainContext = reinterpret_cast<ID3D11DeviceContext *>(renderData.context);
    {
        auto *d3dDevice = reinterpret_cast<ID3D11Device *>(renderData.forwarder);
        ID3D11Texture2D *backBuffer = nullptr;
        if (pSwapChain != nullptr && d3dDevice != nullptr &&
            SUCCEEDED(pSwapChain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D, reinterpret_cast<void **>(&backBuffer))) &&
            backBuffer != nullptr)
        {
            d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_swapChainBackBufferRtv);
            backBuffer->Release();
        }
        // IDXGISwapChain::Present = vtable slot 8 (IUnknown 3 + IDXGIObject 2 +
        // GetPrivateData/GetParent/GetDevice 3).
        g_realSwapChainPresent = reinterpret_cast<void **>(*reinterpret_cast<void **>(pSwapChain))[8];
        g_swapChainPresentHook = new Hooks::FunctionHook<long(void *, std::uint32_t, std::uint32_t)>(
            g_realSwapChainPresent, &ImeApp::SwapChainPresentHook);
        logger::info("Swapchain present hook installed (keeps the overlay above Prisma views)");
    }

    Start(renderData);

    logger::debug("Hooking Skyrim WndProc...");
    RealWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(m_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MainWndProc)));
    if (RealWndProc == nullptr)
    {
        throw SimpleIMEException("Hook WndProc failed!");
    }
    InstallHooks();

    ImeMenu::RegisterMenu();
    ToolWindowMenu::RegisterMenu();
    m_state.SetState(State::StateKey::INITIALIZED);
    // kInputLoaded is sent exactly once (when BSInputDeviceManager initializes)
    // and can arrive before D3DInit finishes — OnInputLoaded would then skip
    // InstallEventSinks with no retry path, leaving them dead for the session.
    // InstallEventSinks() is idempotent (null-guarded), so calling it here too
    // closes the startup-order race either way around.
    Events::InstallEventSinks();
}

void ImeApp::Start(const RE::BSGraphics::RendererData &renderData)
{
    // shared_ptr, NOT a stack object captured by reference: if initialization
    // times out below, Start unwinds and destroys the stack — while the
    // detached IME thread may still call set_value/set_exception on it (UB).
    const auto ensureInitialized = std::make_shared<std::promise<bool>>();
    auto       initialized       = ensureInitialized->get_future();
    // run IME window in a standalone thread
    auto *device  = reinterpret_cast<ID3D11Device *>(renderData.forwarder);
    auto *context = reinterpret_cast<ID3D11DeviceContext *>(renderData.context);

    ImGuiEx::Initialize(m_hWnd, device, context);
    // Copy: the settings keep the user's list untouched; the CJK supplementary
    // fonts are a render-time concern, not part of the persisted config.
    auto userFontList = m_settings.resources.fontPathList;
    // Only extend a non-empty user list: the first entry handed to
    // AddPrimaryFont becomes the PRIMARY font, so appending Malgun Gothic to
    // an empty list would make it primary and strip simplified-Chinese glyphs
    // from the UI. An empty list must fall through to the default list, which
    // already opens with the system message font (e.g. Microsoft YaHei).
    if (!userFontList.empty())
    {
        AppendCjkSupplementaryFonts(userFontList);
    }
    const auto defaultFontFilePathList = GetDefaultFontFilePathList();
    // msyh's native TrueType bytecode compresses CJK glyphs ~12% vertically at
    // UI sizes — bottom strokes snap onto the bitmap's last row and thin out
    // until they read as "chopped off". The FreeType autohinter keeps the full
    // glyph box (verified at 24px 体: native 21 rows vs autohint 24 rows), so
    // force it for every font in the atlas.
    ImGui::GetIO().Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_ForceAutoHint;
    (void)ImGuiEx::AddPrimaryFont(userFontList, defaultFontFilePathList);
    ImGuiEx::M3::Initialize(utils::GetInterfaceFile(Settings::ICON_FILE), m_settings.appearance.schemeConfig);

    std::thread childWndThread([ensureInitialized, this] -> void {
        SetThreadDescription(GetCurrentThread(), L"SimpleIME Message Thread");
        m_imeThreadId = GetCurrentThreadId();
        try
        {
            m_imeWnd.Initialize(m_settings.enableTsf);
            ensureInitialized->set_value(true);
            // we can't call ensureInitialized after create child window, will cause deadlock.
            m_imeWnd.CreateHost(m_hWnd, m_settings);
            ImeWnd::Run();
            // The message loop exited (thread WM_QUIT): destroy the window HERE
            // on the IME thread, so WM_DESTROY → OnDestroy performs the full
            // teardown (task drain, Shutdown, TSF/COM uninit) where the COM
            // apartment actually lives.
            if (const HWND imeHwnd = m_imeWnd.GetHWND(); imeHwnd != nullptr)
            {
                DestroyWindow(imeHwnd);
            }
        }
        catch (...)
        {
            try
            {
                ensureInitialized->set_exception(std::current_exception());
            }
            catch (const std::future_error &)
            {
                // set_exception() on an already-satisfied promise: the failure
                // happened AFTER set_value(true) — i.e. in CreateHost or the
                // message loop, not during Initialize. The future can no longer
                // carry it, so log it here; the thread exits right below.
                logger::critical("SimpleIME worker failed after successful initialization.");
            }
        }
        // Full teardown (window destroy → OnDestroy → UnInitialize) finished —
        // ImeApp::Shutdown waits for this before touching shared state.
        m_imeTeardownDone = true;
    });

    if (initialized.wait_for(INIT_TIMEOUT_SECONDS) == std::future_status::timeout)
    {
        logger::error("IME Window initialization timed out!");

        if (childWndThread.joinable())
        {
            if (!m_imeWnd.SendNotifyMessageToIme(WM_CLOSE, 0, 0))
            {
                logger::warn("IME thread did not respond to WM_CLOSE, detaching...");
            }
            std::thread([t = std::move(childWndThread)]() mutable -> void {
                if (t.joinable())
                {
                    t.join();
                    logger::info("IME child thread successfully joined after timeout.");
                }
            }).detach();
        }
        throw SimpleIMEException("IME Thread initialization timeout.");
    }
    // ready (including the exception case): rethrow a failed Initialize() so
    // D3DInit's catch flips to INITIALIZE_FAILED and runs Shutdown(). Swallowing
    // it here used to leave null m_textService/m_inputMethodManager behind with
    // the state still flipped to INITIALIZED — a guaranteed first-frame crash.
    initialized.get();
    childWndThread.detach();
}

// FIXME: is safe?
void ImeApp::Shutdown()
{
    logger::LogStacktrace();
    m_state.SetState(State::StateKey::SHUTDOWN);
    RequestImeThreadTeardown();
    Uninitialize();
}

bool ImeApp::RequestImeThreadTeardown()
{
    // Both the Shutdown path and the WM_NCDESTROY → Uninitialize path call
    // this; only the first call may post and wait.
    if (m_imeTeardownDone.load(std::memory_order_acquire) || m_imeThreadId.load(std::memory_order_acquire) == 0)
    {
        return true;
    }
    logger::info("Force close ImeWnd...");
    // Post a THREAD message: ImeWnd::Run only exits when PeekMessage retrieves
    // a thread-queue WM_QUIT — sending WM_QUIT to the window (the old
    // SendNotifyMessage path) reaches the WndProc, which ignores it, and the
    // loop kept running while teardown proceeded underneath.
    if (const DWORD imeThreadId = m_imeThreadId.load(); !PostThreadMessageW(imeThreadId, WM_QUIT, 0, 0))
    {
        logger::error("Can't close ImeWnd! May IME uninitialized?");
    }
    // The worker tears itself down on the IME thread (WM_QUIT → loop exit →
    // window destroy → OnDestroy → UnInitialize). Wait — bounded — so the
    // game-thread Uninitialize() below does not race it (ImGui and TSF are not
    // thread-safe). If the worker is stuck, proceed after 2s: it matched the
    // old behavior of an unstoppable thread, minus the undefined teardown.
    for (int i = 0; i < 200 && !m_imeTeardownDone.load(); ++i)
    {
        Sleep(10);
    }
    if (!m_imeTeardownDone.load())
    {
        logger::warn("IME thread did not finish teardown within 2s, continuing on the game thread.");
    }
    return m_imeTeardownDone.load();
}

void ImeApp::SaveSettings()
{
    // Stamp the last in-game observed 中/英 state into the runtime cache
    // before the write. Only when this session actually observed a mode: the
    // boot-time WM_NCACTIVATE churn can trigger a save before any observation,
    // and stamping the untouched default over the loaded value would erase the
    // previous session's prior. (After the startup seed the mode counts as
    // observed and the stamp writes the seeded value back unchanged.)
    auto &conversionState = Core::State::GetInstance();
    if (conversionState.HasObservedConversionMode())
    {
        m_settings.input.lastNativeConversion = conversionState.GetConversionMode().IsNative();
    }
    SettingsManager::Save(m_settings);
}

void ImeApp::InstallHooks()
{
    Hooks::Scaleform::Install();
    // The present hook is the fallback frame driver for Meridian sessions
    // (see PresentHook) — safe to install once at startup next to the others.
    if (g_PresentHook == nullptr)
    {
        // Same single-threaded-install invariant as the D3DInit hook: the
        // detour goes live inside the ctor before hookData is assigned.
        g_PresentHook = std::make_unique<Hooks::D3DPresentHookData>(&ImeApp::PresentHook);
        logger::debug("Installed D3D present hook (Meridian fallback frame driver)");
    }
}

void ImeApp::UninstallHooks()
{
    Hooks::Scaleform::Uninstall();
    // NOTE: the present hook is intentionally NOT uninstalled — the detour
    // cannot be safely removed during teardown (render thread), and the
    // trampoline holding Original() must stay alive for the engine's present
    // to keep executing. PresentHook degrades to a passthrough on its own:
    // after Uninitialize the state gate and the (uninstalled) bridges make
    // every branch inert.
}

void ImeApp::Draw()
{
    if (!m_state.IsInitialized())
    {
        return;
    }
    // Serialize against the present-hook fallback driver (see PresentHook).
    if (!g_ImGuiFrameMutex.try_lock())
    {
        return;
    }
    std::lock_guard frameLock(g_ImGuiFrameMutex, std::adopt_lock);
    g_lastUiFrameMs = GetTickCount64();

    ImGuiEx::NewFrame();

    m_imeWnd.Draw(m_settings);

    ImGuiEx::EndFrame();
    ImGuiEx::Render();
}

void ImeApp::PresentHook(std::uint32_t a_unk)
{
    const auto now = GetTickCount64();
    // The normal PostDisplay frame path owns rendering while it is alive; the
    // present hook only steps in when it has gone silent (Meridian sessions)
    // or when a Meridian view needs its per-frame bridge work.
    const bool meridianSession = Hooks::MeridianBridge::HasFocus() && !Hooks::PrismaBridge::OwnsInput();
    const bool uiFrameStale    = now - g_lastUiFrameMs.load() > PRESENT_FRAME_TAKEOVER_MS;
    if (meridianSession && uiFrameStale && g_instance != nullptr && g_instance->m_state.IsInitialized() &&
        g_ImGuiFrameMutex.try_lock())
    {
        std::lock_guard frameLock(g_ImGuiFrameMutex, std::adopt_lock);
        // Deliberately NOT refreshing g_lastUiFrameMs here: the token measures
        // PostDisplay liveness only. Refreshing it would stand this hook down
        // again for PRESENT_FRAME_TAKEOVER_MS, re-running one frame per 250ms
        // instead of every present. When PostDisplay resumes, Draw() refreshes
        // the token and the hook stands down on its own.
        ImGuiEx::NewFrame();
        g_instance->m_imeWnd.Draw(g_instance->m_settings);
        ImGuiEx::EndFrame();
        ImGuiEx::Render();
        // State-change log only: this would otherwise fire every frame for a
        // whole Meridian session.
        static std::atomic<bool> s_presentDriveAnnounced{false};
        if (!s_presentDriveAnnounced.exchange(true))
        {
            logger::info("PostDisplay went silent during a Meridian session; the present hook is now driving the frames");
        }
    }
    // The bridge tick MUST run every frame while a Meridian view is focused:
    // it captures the focused DOM field and flushes queued composition text.
    // It used to live in ImeMenu::PostDisplay only, which the engine stops
    // calling during Meridian sessions — committed text sat in the queue
    // forever and nothing was captured.
    Hooks::MeridianBridge::Tick();
    if (g_PresentHook != nullptr)
    {
        g_PresentHook->Original(a_unk);
    }
}

auto ImeApp::SwapChainPresentHook(void *swapChain, std::uint32_t syncInterval, std::uint32_t flags) -> long
{
    // Layering, not frame-driving: during a Prisma takeover (PMCM search box,
    // Outfit Wheeler fields) PrismaUI renders its Ultralight views from its own
    // present CALL-SITE hook — which runs after the game's menu stage, where
    // our PostDisplay overlay draws — so their views cover the candidate
    // window no matter what we draw earlier. SKSEMF framework menus out-draw
    // ImeMenu::PostDisplay the same way (their render callback sits above
    // ImeMenu in the frame), so a candidate overlapping a framework menu hides
    // under it. The real swapchain present is the last draw of the frame;
    // rendering into the backbuffer right before the flip puts our overlay
    // back on top. Gated on the two surfaces known to out-draw PostDisplay so
    // every other surface (Scaleform menus, Meridian, ENB) keeps its layering.
    if ((Hooks::PrismaBridge::ShouldRoute() || Hooks::SkseMenuFrameworkBridge::SessionActive()) &&
        g_instance != nullptr && g_instance->m_state.IsInitialized() && g_ImGuiFrameMutex.try_lock())
    {
        std::lock_guard frameLock(g_ImGuiFrameMutex, std::adopt_lock);
        // ImGui_ImplDX11 draws into whatever render target is bound; the game
        // may leave it unbound on its present path, so pin the backbuffer.
        if (g_swapChainContext != nullptr && g_swapChainBackBufferRtv != nullptr)
        {
            ID3D11RenderTargetView *bound = nullptr;
            g_swapChainContext->OMGetRenderTargets(1, &bound, nullptr);
            if (bound == nullptr)
            {
                ID3D11RenderTargetView *rtv = g_swapChainBackBufferRtv;
                g_swapChainContext->OMSetRenderTargets(1, &rtv, nullptr);
            }
            else
            {
                bound->Release();
            }
        }
        ImGuiEx::NewFrame();
        g_instance->m_imeWnd.Draw(g_instance->m_settings);
        ImGuiEx::EndFrame();
        ImGuiEx::Render();
    }
    return (*g_swapChainPresentHook)(swapChain, syncInterval, flags);
}

auto ImeApp::MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) -> LRESULT
{
    auto &app = GetInstance();
    // Prisma announces that it is associating (or disassociating) its own IME
    // context with the game window; coordinate with it so the two never fight
    // over the keyboard. A registered message (> WM_APP), checked before the
    // switch; wParam mirrors Prisma's SetAssociation flag (1/0).
    if (const unsigned prismaAssociation = Hooks::PrismaBridge::AssociationMessage();
        prismaAssociation != 0 && uMsg == prismaAssociation)
    {
        Hooks::PrismaBridge::OnAssociationMessage(wParam != 0);
    }
    switch (uMsg)
    {
        case WM_NCACTIVATE:
            if (wParam == TRUE)
            {
                ImeController::GetInstance()->SyncImeState();
            }
            break;
        case WM_IME_SETCONTEXT:
            // The game window has no associated HIMC while SimpleIME is enabled
            // (DoEnableMod detached it), so this message never actually fires —
            // the Meridian focus reclaim therefore lives in ImeWnd's
            // WM_KILLFOCUS, which does fire on every steal. Keep the lParam=0
            // suppression for the case another mod re-attaches a context.
            return ::DefWindowProc(hWnd, uMsg, wParam, 0);
        case WM_NCDESTROY: {
            // Forward to the game's own WndProc: Uninitialize() restores it as
            // the window's proc, and the engine must still see the destruction
            // of its own window. Calling it directly is required because the
            // WndProc swap above does not redirect the message already being
            // dispatched into this function, and RealWndProc is nulled after.
            const WNDPROC gameProc = RealWndProc;
            app.Uninitialize();
            return gameProc != nullptr ? gameProc(hWnd, uMsg, wParam, lParam) : ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
        case WM_INPUTLANGCHANGE: {
            // Sent after the game thread's keyboard layout actually changed —
            // the confirmation point for ForceEnglishKeyboardOnGameThread()'s
            // WM_INPUTLANGCHANGEREQUEST.
            const auto hkl = reinterpret_cast<HKL>(lParam);
            logger::info("Game window input language changed, layout={:p}", static_cast<void *>(hkl));
            // Watchdog: Windows (and some IMEs) re-apply the window's remembered
            // input method — the user's Chinese TIP — on activation changes, even
            // long after the IME was disabled (observed in the game log: the game
            // thread drifting back to 0x08040804 ~20s after a clean disable). That
            // resurrects the "still in IME state" symptom: the Chinese IME is
            // active on the game thread again and pops candidates on WASD. While
            // the mod is enabled but the IME is disabled, the game must receive
            // raw keys, so re-request the English layout whenever the game thread
            // drifts back to a non-English one. Loop-safe: the re-request ends in
            // an English WM_INPUTLANGCHANGE, which fails the check below. When the
            // mod itself is disabled we must not fight the user's own input method,
            // hence the IsModEnabled() gate.
            if (ImeController::GetInstance()->IsModEnabled() && Core::State::GetInstance().ImeDisabled())
            {
                const auto langid = static_cast<LANGID>(reinterpret_cast<uintptr_t>(hkl) & 0xFFFF);
                if (hkl != nullptr && PRIMARYLANGID(langid) != PRIMARYLANGID(LANGID_ENG))
                {
                    // HKL-level re-assert. Loop-safe: the request ends in an
                    // English WM_INPUTLANGCHANGE, which fails the check above.
                    // (A TSF-level activation on this thread is impossible: the
                    // game thread runs an MTA, where TSF is unsupported — and
                    // moot anyway, since user-visible composition happens on the
                    // IME thread's own document.)
                    if (const HKL hklEnglish = LoadKeyboardLayoutW(L"00000409", 0); hklEnglish != nullptr && hklEnglish != hkl)
                    {
                        PostMessageW(hWnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hklEnglish));
                    }
                    logger::info("Game thread drifted to a non-English layout while the IME is disabled, re-asserting English");
                }
            }
            break;
        }
        default:
            break;
    }
    return RealWndProc(hWnd, uMsg, wParam, lParam);
}
} // namespace Ime
