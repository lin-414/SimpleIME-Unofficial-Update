#include "ImeWnd.hpp"

#include "configs/CustomMessage.h"
#include "core/State.h"
#include "hooks/MeridianBridge.h"
#include "hooks/PrismaBridge.h"
#include "i18n/translator_manager.h"
#include "icons.h"
#include "ime/ITextServiceFactory.h"
#include "ime/ImeController.h"
#include "imgui_impl_win32.h"
#include "imguiex/ErrorNotifier.h"
#include "imguiex/Material3.h"
#include "imguiex/imguiex_enum_wrap.h"
#include "imguiex/imguiex_m3.h"
#include "log.h"
#include "menu/MenuNames.h"
#include "ui/ImGuiInputState.h"
#include "ui/ImeOverlay.h"
#include "ui/LanguageBar.h"
#include "ui/fonts/FontManager.h"
#include "utils/Utils.h"
#include "WCharUtils.h"

#include <msctf.h>
#include <windows.h>
#include <windowsx.h>

#include <filesystem>
#include <utility>

#include <algorithm>
#include <array>

namespace Ime
{
namespace Global
{
extern HINSTANCE g_hModule;
}

namespace
{
/// How long after a composition commit the WM_CHAR branch still drops an echo
/// (see InitializeTextService). The echo itself arrives within one message-loop
/// turn; the window only bounds the rare TIP that commits from a posted
/// message after its own echo already passed the still-armed composing gate.
constexpr auto COMMIT_ECHO_DROP_WINDOW_MS = 100U;

/// A bare Shift tap (keydown→keyup with no other key in between) within this
/// window counts as the IME's 中/英 toggle hotkey press. WeChat IME publishes
/// its mode through no TSF compartment at all (all six watchpoints stay
/// unwritten — see DumpConversionCompartments), so the displayed state is
/// predicted optimistically here; the behavioral inference (composition start /
/// raw letter WM_CHAR) corrects it on the next keystroke if the active IME
/// does not actually toggle on Shift.
constexpr auto SHIFT_TAP_WINDOW_MS = 1000U;

/// IME-thread only: the ImeWnd whose text service is live, set in
/// InitializeTextService and cleared in UnInitialize. The commit callback is a
/// plain function pointer (no captures), so it reaches the instance through
/// this file-static sink.
ImeWnd *s_commitEchoSink = nullptr;

auto GetThis(HWND hWnd) -> ImeWnd *
{
    const auto ptr = GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (ptr == 0) return nullptr;
    return reinterpret_cast<ImeWnd *>(ptr);
}

auto RegisterImeWindowClass(WNDPROC wndProc) -> bool
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_PARENTDC;
    wc.cbClsExtra    = 0;
    wc.lpfnWndProc   = wndProc;
    wc.cbWndExtra    = 0;
    wc.lpszClassName = g_MainClassName;
    wc.hInstance     = Global::g_hModule;

    WNDCLASSEXW existingClass{};
    if (GetClassInfoExW(Global::g_hModule, wc.lpszClassName, &existingClass) == FALSE)
    {
        return RegisterClassExW(&wc) != 0;
    }
    return true;
}

//! DPI awareness for support different monitor that has different physical DPI, avoid blurry when move between
//! monitors.
// ！@see WndProc::WM_DIPCHANGED
void TryEnableImeWndDpiAware()
{
    logger::info("Try to enable DPI aware for IME Wnd...");
    using PFN_SetThreadDpiAwarenessContext = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);

    HMODULE    hUser32 = GetModuleHandleW(L"user32.dll");
    const auto pSetThreadDpiAwarenessContext =
        reinterpret_cast<PFN_SetThreadDpiAwarenessContext>(GetProcAddress(hUser32, "SetThreadDpiAwarenessContext"));

    if (pSetThreadDpiAwarenessContext != nullptr)
    {
        pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        logger::info("Enable DPI aware successful!");
    }
}

inline auto WantToOpenOverlay(Settings &settings, const bool shortcutPressed)
{
    return shortcutPressed || settings.runtimeData.requestShowOverlay;
}

/**
 * The toolwindow and ImeWnd handle the shortcut at the same time.
 * - ToolWindow already released ? -> ImeWnd handle shortcut to response the open ToolWindow request.
 *                      alive    ? -> ToolWindow handle shortcut to response the open/pin/unpin/close ToolWindow request.
 * - Debounce timer passed? -> If toolwindow is alive, close it and release translator; otherwise, open toolwindow and load translator.
 */
inline void ManageImeOverlayOnDemand(
    std::unique_ptr<UI::ImeOverlay> &imeOverlay, DebounceTimer &debounceTimer, Settings &settings, const bool shortcutPressed)
{
    bool shouldOpenImeOverlay = false;
    if (imeOverlay == nullptr && WantToOpenOverlay(settings, shortcutPressed))
    {
        shouldOpenImeOverlay = true;
    }
    // pass the first call
    if (!debounceTimer.IsWaiting() || debounceTimer.Check())
    {
        if (imeOverlay != nullptr)
        {
            imeOverlay.reset();
        }
        else if (shouldOpenImeOverlay)
        {
            imeOverlay = std::make_unique<UI::ImeOverlay>(settings.appearance.language);
        }
    }
}
} // namespace

ImeWnd::~ImeWnd()
{
    // Intentionally NO UnInitialize()/DestroyWindow here: this destructor runs
    // during static destruction on the game thread, while the IME thread owns
    // the window, its COM apartment and every TSF object — tearing those down
    // cross-thread is undefined (and CoUninitialize would balance an apartment
    // this thread never initialized). The full teardown runs on the IME thread
    // in OnDestroy (WM_DESTROY), which the ImeApp::Start worker performs after
    // its message loop exits.
}

void ImeWnd::InitializeTextService()
{
    m_textService = TextServiceFactory::Create(m_fEnabledTsf);
    // Commit-echo bookkeeping: a commit is triggered by a keystroke (space,
    // enter, candidate pick) whose WM_CHAR echo TranslateMessage posted BEFORE
    // the keydown dispatch that performed the commit — so the echo is
    // dispatched AFTER OnEndComposition has already cleared IN_COMPOSING and
    // the composition-active gate in the WM_CHAR branch can no longer catch
    // it. Stamp the commit time here (same thread as the WM_CHAR branch); that
    // branch drops the echo as the first WM_CHAR inside the window.
    s_commitEchoSink = this;
    m_textService->RegisterCallback([](std::wstring_view compositionString) static -> void {
        if (s_commitEchoSink != nullptr)
        {
            s_commitEchoSink->m_lastCommitTickMs = GetTickCount64();
        }
        Skyrim::SendUiString(compositionString);
    });
}

void ImeWnd::Initialize(const bool enableTsf) noexcept(false)
{
    // A previous cycle's UnInitialize latched the teardown flag; this run
    // rebuilds everything it guards on a fresh IME thread.
    m_fTearingDown = false;
    TryEnableImeWndDpiAware();
    if (!RegisterImeWindowClass(WndProc))
    {
        throw SimpleIMEException("Can't register class");
    }
    m_fEnabledTsf = enableTsf;

    auto &tsfSupport = Tsf::TsfSupport::GetSingleton();
    if (FAILED(tsfSupport.InitializeTsf(true)))
    {
        m_fEnabledTsf = false;
    }

    InitializeTextService();
    m_imeWindow = std::make_unique<ImeWindow>();

    m_inputMethodManager = new InputMethodManager();
    if (FAILED(m_inputMethodManager->Initialize(tsfSupport.GetThreadMgr(), tsfSupport.GetTfClientId())))
    {
        throw SimpleIMEException("Can't initialize LangProfileUtil");
    }
    // The activation sink delegates conversion-mode re-reads to the text
    // service: the live 中/英 value sits on the focused context's compartment
    // (owned by the text service), not on the thread-level one the sink could
    // read itself. Lambda holds a raw pointer on purpose — both objects share
    // the ImeWnd lifetime, and the null-check covers teardown order.
    const auto textServicePtr = m_textService.get();
    m_inputMethodManager->SetConversionModeRefresher([textServicePtr]() -> void {
        if (textServicePtr != nullptr)
        {
            textServicePtr->RefreshConversionMode();
        }
    });
}

void ImeWnd::UnInitialize() noexcept
{
    // Published before anything is released: Draw() runs on the render thread and
    // dereferences the members torn down below (m_imeOverlay, the text service and
    // the profile list behind m_inputMethodManager), so it must observe this
    // before the first release happens.
    m_fTearingDown = true;
    m_imeOverlay.reset(); // must release before ImGui shutdown
    s_commitEchoSink      = nullptr;
    if (m_textService != nullptr)
    {
        m_textService->UnInitialize();
    }
    if (m_inputMethodManager != nullptr)
    {
        m_inputMethodManager->UnInitialize();
    }
    Tsf::TsfSupport::GetSingleton().UnInitializeTsf();
}

void ImeWnd::CreateHost(HWND hWndParent, Settings &settings)
{
    logger::info("Start ImeWnd Thread...");
    m_hWnd = CreateWindowExW(0, g_MainClassName, L"Hide", WS_CHILD, 0, 0, 0, 0, hWndParent, nullptr, Global::g_hModule, this);
    if (m_hWnd == nullptr)
    {
        throw SimpleIMEException("Create ImeWnd failed");
    }
    OnCreated(settings);
}

/**
 * ## Why this is a plain Win32 PeekMessage loop and not ITfMessagePump
 *
 * ITfMessagePump is a TSF wrapper around GetMessage/PeekMessage that adds pre- and
 * post-processing hooks so the TSF manager can intercept preserved keys (e.g. Shift for
 * Microsoft Pinyin's CN/EN toggle) before they reach the application, and ITfKeystrokeMgr
 * routes those intercepted keys to the active text service.
 *
 * In a normal text editor that makes sense. Here it does not:
 *
 * 1. **Skyrim does not expose keyboard messages to mods.** The base game handles all raw
 *    input internally; WM_KEYDOWN/WM_KEYUP never reach mod-side code through the normal
 *    game loop. The only keyboard signal that matters is WM_CHAR, already handled by the
 *    WndProc WM_CHAR branch. ITfKeystrokeMgr adds nothing to that path.
 *
 * 2. **Microsoft Pinyin (mspy) has a confirmed OS-level bug when ITfMessagePump and
 *    ITfKeystrokeMgr are used together.** When the user presses Shift during an active
 *    composition (mspy's preserved key for CN/EN mode toggle), mspy triggers a nested
 *    message loop inside the ITfMessagePump pre-processing stage. That nested loop devours
 *    all subsequent messages until AssociateFocus is called to clear the document focus,
 *    effectively freezing the application. A plain PeekMessage loop bypasses the TSF
 *    pre-processing layer entirely and avoids the hang.
 *
 *    Known Microsoft bug, acknowledged in KB4564002:
 *    https://support.microsoft.com/en-us/topic/kb4564002-you-might-have-issues-on-windows-10-version-20h2-and-windows-10-version-2004-when-using-some-microsoft-imes-63696506-47d2-9997-0b72-41a68e328692
 *
 *    Independently reported by multiple projects:
 *    - KiCad issue #9882 (open since 2021, marked out-of-scope — Microsoft's bug):
 *      https://gitlab.com/kicad/code/kicad/-/issues/9882
 *    - Audacity issue #1618:
 *      https://github.com/audacity/audacity/issues/1618
 *
 * Do not reintroduce the TSF message pump here.
 */
void ImeWnd::Run()
{
    MSG  msg  = {};
    bool done = false;
    while (!done)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (msg.message == WM_QUIT)
            {
                done = TRUE;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!done)
        {
            // Block until the next message (posted, sent, or thread-queued —
            // all of them wake WaitMessage) instead of polling every 10ms.
            // Messages arriving between the drain above and this call are
            // already in the queue, so WaitMessage returns immediately; its
            // documented spurious wake-ups just loop back into the drain.
            WaitMessage();
        }
    }

    logger::info("Exit ImeWnd Thread...");
}

auto ImeWnd::Focus() const -> void
{
    if (!IsFocused())
    {
        SetFocus(m_hWnd); // The return value only indicates which HWND had the focus previously.
    }
}

auto ImeWnd::FocusTextService(const bool focus) const -> bool
{
    return m_textService->OnFocus(focus);
}

auto ImeWnd::ToggleKeyboard(const bool open) const -> void
{
    m_textService->ToogleKeyboard(open);
}

auto ImeWnd::IsFocused() const -> bool
{
    return m_fFocused;
}

auto ImeWnd::SendNotifyMessageToIme(UINT uMsg, WPARAM wParam, LPARAM lParam) const -> bool
{
    if (m_hWnd == nullptr)
    {
        // No window to notify: report "not delivered" so callers take their
        // failure path instead of assuming the IME thread got the message.
        return false;
    }
    return SendNotifyMessageW(m_hWnd, uMsg, wParam, lParam) != FALSE;
}

auto ImeWnd::ActivateLanguageProfile(const GUID &guidProfile) const -> HRESULT
{
    return m_inputMethodManager->ActivateProfile(guidProfile);
}

void ImeWnd::AbortIme() const
{
    if (State::GetInstance().HasAny(State::IN_CAND_CHOOSING, State::IN_COMPOSING))
    {
        logger::info("Aborting IME composition and releasing keyboard focus");
    }
    // Terminate any active composition on the IME thread, where the TSF/IMM32
    // objects live. Sent unconditionally: the state flags can be stale, and
    // TerminateComposition is a no-op when nothing is composing. SendNotifyMessage
    // runs the WndProc inline on the IME thread (e.g. from EnableIme(false)) and
    // posts asynchronously from the game UI thread (e.g. ImeMenu click-to-abort).
    SendNotifyMessageToIme(CM_ABORT_IME, 0, 0);
    // Return Win32 focus to the game window — only possible from the game UI
    // thread. SetFocus to a window owned by another thread fails unless the
    // input queues are attached (ImeManager::Focus does that dance; the disable
    // path calls it right after us, so skipping here is correct).
    if (GetCurrentThreadId() == m_gameThreadId)
    {
        SetFocus(m_hWndParent);
    }
}

void ImeWnd::Draw(Settings &settings)
{
    // The IME thread may already be tearing the window down (see UnInitialize).
    // Everything below dereferences members it releases, so bail out. The null
    // checks are a backstop for a half-failed Initialize() that somehow got
    // past Start()'s exception propagation.
    if (m_fTearingDown || m_textService == nullptr || m_imeWindow == nullptr || m_inputMethodManager == nullptr)
    {
        return;
    }
    if (std::exchange(settings.runtimeData.requestMonitorScale, false))
    {
        m_uiScale            = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hWnd);
        m_fWantUpdateUiScale = true;
    }
    if (m_fWantUpdateUiScale.exchange(false))
    {
        ImGuiEx::M3::Context::GetM3Styles().UpdateScaling(m_uiScale.load());
        // The rescaled theme also has to reach the Meridian DOM panel.
        Hooks::MeridianBridge::RequestUiThemeRefresh();
    }
    // The base ImGui style must stay in lockstep with the M3 palette, and the
    // only safe moment to sync it is the frame start: theme switches rebuild
    // the scheme mid-frame (mode selector, theme builder, reset), and a
    // mid-frame refresh is undone by every style guard still open across it —
    // PopStyleColor restores the slot value captured before the refresh, which
    // left the settings page canvas in the old theme while the palette moved
    // on. Deriving the style here, before any window or guard exists, makes
    // the pair consistent again within one frame no matter who changed what.
    ImGuiEx::M3::SetupDefaultImGuiStyles(ImGui::GetStyle());
    // Mirror the candidate window's M3 theme onto the Meridian DOM panel when
    // a refresh was requested. Must run here: the M3 styles and the ImGui
    // style are render-thread-only state, and this is the one place they are
    // valid outside ImeWindow's own drawing.
    if (Hooks::MeridianBridge::ConsumeUiThemeRefreshRequested())
    {
        const auto          &style  = ImGui::GetStyle();
        const auto          &m3     = ImGuiEx::M3::Context::GetM3Styles();
        const auto          &scheme = m3.Colors();
        const auto           at     = [&](ImGuiEx::M3::Spec::ColorRole role) { return scheme[role]; };
        const auto           toArr  = [](const ImVec4 &c) { return std::array<float, 4>{c.x, c.y, c.z, c.w}; };
        Hooks::MeridianBridge::UiThemePalette palette{};
        palette.windowBg         = toArr(style.Colors[ImGuiCol_WindowBg]);
        palette.text             = toArr(at(ImGuiEx::M3::Spec::ColorRole::onSurface));
        palette.caret            = toArr(at(ImGuiEx::M3::Spec::ColorRole::primary));
        palette.divider          = toArr(at(ImGuiEx::M3::Spec::ColorRole::outlineVariant));
        palette.numberText       = toArr(at(ImGuiEx::M3::Spec::ColorRole::onSurfaceVariant));
        palette.chipHoverBg      = toArr(at(ImGuiEx::M3::Spec::ColorRole::surfaceContainerHigh));
        palette.chipSelectedBg   = toArr(at(ImGuiEx::M3::Spec::ColorRole::primary));
        palette.chipSelectedText = toArr(at(ImGuiEx::M3::Spec::ColorRole::onPrimary));
        palette.rowSelectedBg    = toArr(at(ImGuiEx::M3::Spec::ColorRole::primary));
        palette.rowSelectedText  = toArr(at(ImGuiEx::M3::Spec::ColorRole::onPrimary));
        palette.windowRounding   = style.WindowRounding;
        palette.scale            = m3.GetPixels(1.0F);
        // The effective primary font: the first loadable configured font, else
        // the system default — the same resolution AddPrimaryFont applies to
        // the ImGui atlas, so the DOM panel renders with the same face.
        for (const auto &fontPath : settings.resources.fontPathList)
        {
            std::error_code ec;
            // The config stores font paths as UTF-8; the plain string ctor
            // would decode them in the ANSI codepage and disagree with the
            // UTF-8 decoding every other consumer applies.
            if (std::filesystem::exists(std::filesystem::u8path(fontPath), ec))
            {
                palette.primaryFontPath = fontPath;
                break;
            }
        }
        if (palette.primaryFontPath.empty())
        {
            palette.primaryFontPath = WCharUtils::ToString(GetDefaultFontFilePath());
        }
        Hooks::MeridianBridge::ConsumeUiThemeRefresh(palette);
    }
    // Replay the focus loss recorded by WM_KILLFOCUS on the IME thread. The keys
    // must be dropped here rather than in the WndProc: ImGui input state is
    // owned by this thread and writing it from the WndProc raced the frame in
    // progress. Needed because ImeMenu is the only source of ImGui key events,
    // and a key held across the focus change never gets its key-up (the release
    // goes to the window that took the focus) — ImGui would keep it down forever.
    if (m_fWantClearInput.exchange(false))
    {
        ImGui::GetIO().ClearInputKeys();
    }
    // Consume the one-frame latch armed by the Behaviour panel's shortcut-capture
    // widget (it re-arms it later this frame while still capturing): the chord
    // evaluation below runs before the capture widget, so pass the snapshot down.
    const bool swallowShortcutToggle = std::exchange(settings.runtimeData.swallowShortcutToggle, false);
    HealStuckShortcutKeys(settings.shortcut);
    HealStuckMouseButtons();
    // Evaluate the toggle chord exactly once per frame, up here: IsKeyChordPressed
    // is edge-based and would otherwise report "pressed" to both the open check
    // (ManageImeOverlayOnDemand) and ImeOverlay::Draw within the same frame.
    const bool shortcutPressed = !swallowShortcutToggle && ImGui::IsKeyChordPressed(settings.shortcut);
    m_textService->UpdateIfDirty();

    UpdateMouseCursorVisual();

    ManageImeOverlayOnDemand(m_imeOverlay, m_translatorLoadDebounceTimer, settings, shortcutPressed);

    {
        auto      &m3Styles  = ImGuiEx::M3::Context::GetM3Styles();
        const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
        DrawImeStates(); // draw ing ImGui default Debug window;
        ErrorNotifier::GetInstance().Show();
        m_imeWindow->Draw(m_textService->GetCompositionInfo(), m_textService->GetCandidateUi(), settings);

        const auto &activeLang   = m_inputMethodManager->GetActiveLangProfile();
        const auto &langProfiles = m_inputMethodManager->GetLangProfiles();

        if (m_imeOverlay != nullptr)
        {
            m_imeOverlay->Draw(activeLang, langProfiles, settings, shortcutPressed);
            if (settings.runtimeData.overlayShowing)
            {
                m_translatorLoadDebounceTimer.Poke();
            }
        }
    }
    ImeController::GetInstance()->SaveSettings(settings);
}

/// While a Prisma view owns input (PMCM search box, Outfit Wheeler fields),
/// its editing keys arrive exclusively through the game window's message
/// stream — which goes quiet the moment our IME takes the Win32 focus.
/// Forward the editing keys so backspace/arrows/enter keep working in the
/// field while our IME owns the keyboard. During a composition the IME
/// consumes these keys itself (candidate navigation), so hold off until it
/// ends; the text streams need no help (SendUiString's Prisma route delivers
/// committed characters, and the WM_CHAR branch below feeds English typing).
/// Called for WM_KEYDOWN and WM_KEYUP so Ultralight sees matched pairs.
void ForwardEditingKeyToPrismaHostIfOwned(HWND hWndParent, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static constexpr std::array<WORD, 10> kForwardedKeys = {
        VK_BACK, VK_RETURN, VK_DELETE, VK_TAB,  VK_UP,
        VK_DOWN, VK_LEFT,   VK_RIGHT,  VK_HOME, VK_END
    };
    if (std::ranges::find(kForwardedKeys, static_cast<WORD>(wParam)) == kForwardedKeys.end())
    {
        return;
    }
    if (!Hooks::PrismaBridge::ShouldRoute() ||
        Core::State::GetInstance().HasAny(Core::State::IN_COMPOSING, Core::State::IN_CAND_CHOOSING))
    {
        return;
    }
    PostMessageW(hWndParent, uMsg, wParam, lParam);
}

auto ImeWnd::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) -> LRESULT
{
    // logger::debug("Message: {:#X} {} {}", uMsg, wParam, lParam);
    // Meridian sessions are click-driven: the click into its text field hands
    // Win32 focus to the game window, whose IME context SimpleIME detached
    // (DoEnableMod) — the composition surface dies with the focus and the
    // game window itself never sees WM_IME_SETCONTEXT (no HIMC to attach it
    // to), so the steal is otherwise silent. Pull the focus straight back:
    // deferred one hop out of the focus transition, re-checked at handling
    // time, and skipped when the focus went somewhere legitimate (alt-tab /
    // Steam overlay take the foreground with them).
    if (const UINT reclaimMsg = ReclaimImeFocusMessage(); reclaimMsg != 0 && uMsg == reclaimMsg)
    {
        if (ImeWnd *reclaim = GetThis(hWnd); reclaim != nullptr)
        {
            reclaim->ReclaimImeFocus(hWnd);
        }
        return 0;
    }
    ImeWnd *pThis = GetThis(hWnd);
    if (pThis != nullptr)
    {
        if (pThis->m_textService->ProcessImeMessage(hWnd, uMsg, wParam, lParam))
        {
            return 0;
        }
    }

    switch (uMsg)
    {
        HANDLE_MSG(hWnd, WM_NCCREATE, OnNccCreate);
        case WM_CREATE: {
            if (pThis == nullptr) break;
            return 0;
        }
        case WM_KEYDOWN:
            // Diagnostic: does the physical keyboard reach the composition
            // surface at all? The Meridian session log showed WeChat's Shift
            // toggle reacting while letters never arrived — this line pins
            // down which keys reach ImeWnd.
            logger::debug("ImeWnd WM_KEYDOWN vk={:#x}", wParam);
            if (pThis != nullptr)
            {
                pThis->HandleKeyDown(uMsg, wParam, lParam);
            }
            break;
        case WM_KEYUP:
            if (pThis != nullptr)
            {
                pThis->HandleKeyUp(uMsg, wParam, lParam);
            }
            break;
        case WM_DESTROY: {
            if (pThis == nullptr) break;
            ImmAssociateContextEx(hWnd, nullptr, IACE_DEFAULT);
            return pThis->OnDestroy();
        }
        case WM_SETTINGCHANGE: {
            if (pThis == nullptr) break;
            pThis->m_uiScale            = ImGui_ImplWin32_GetDpiScaleForHwnd(hWnd);
            pThis->m_fWantUpdateUiScale = true;
            break; // let DefWindowProcW below finish the broadcast handling
        }
        case WM_DPICHANGED: {
            if (pThis == nullptr) break;
            const float g_dpi           = HIWORD(wParam);
            const auto  scale           = g_dpi / USER_DEFAULT_SCREEN_DPI;
            pThis->m_uiScale            = scale;
            pThis->m_fWantUpdateUiScale = true;
            return 0;
        }
        case CM_EXECUTE_TASK: {
            TaskQueue::GetInstance().ExecuteImeThreadTasks();
            return 0;
        }
        case CM_ABORT_IME: {
            if (pThis == nullptr) break;
            // Runs on the IME thread (WndProc of the IME window), so it is safe
            // to touch the TSF/IMM32 text service here.
            pThis->m_textService->AbortIme();
            return 0;
        }
        case WM_IME_SETCONTEXT:
            lParam &= ~(ISC_SHOWUICOMPOSITIONWINDOW | ISC_SHOWUICANDIDATEWINDOW);
            return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        case WM_SETFOCUS:
            if (pThis == nullptr) break;
            pThis->m_fFocused = true;
            logger::debug("IME window get focus.");
            return 0;
        case WM_KILLFOCUS: {
            if (pThis == nullptr) break;
            pThis->m_fFocused      = false;
            pThis->m_fWantClearInput = true; // consumed by Draw() on the render thread (ImGui is single-threaded)
            logger::debug("IME window lost focus.");
            // Terminate an active composition NOW, here on the IME thread and
            // under our control: AbortIme clears the editor first, so the
            // composition-end TSF fires for this focus change delivers an empty
            // string instead of injecting the partial composition through
            // SendUiString while the OS is mid focus transition. That injection
            // (plus the TIP tearing down its candidate UI around it) was the
            // suspected crash window of the old FIXME; ending the
            // composition on our own terms closes it.
            if (State::GetInstance().HasAny(State::IN_COMPOSING, State::IN_CAND_CHOOSING))
            {
                logger::debug("Focus lost during composition, aborting it on the IME thread.");
                pThis->m_textService->AbortIme();
            }
            // Meridian focus reclaim: the click into a Meridian text field moves Win32 focus
            // to the game window (no WM_KILLFOCUS-free path exists). Deferred one hop; the
            // handler re-checks every guard.
            if (Hooks::MeridianBridge::HasFocus() && !Core::State::GetInstance().ImeDisabled() &&
                !Hooks::PrismaBridge::OwnsInput() && GetForegroundWindow() == pThis->m_hWndParent)
            {
                PostMessageW(hWnd, ReclaimImeFocusMessage(), 0, 0);
            }
            return 0;
        }
        case WM_CHAR: {
            if (pThis == nullptr) break;
            return pThis->HandleCharMessage(wParam, lParam);
        }
        default:
            // ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
            break;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

auto ImeWnd::ReclaimImeFocusMessage() -> UINT
{
    static const UINT id = RegisterWindowMessageW(L"SimpleIME.ReclaimImeFocus.v2");
    return id;
}

void ImeWnd::ReclaimImeFocus(HWND hWnd)
{
    if (Hooks::MeridianBridge::HasFocus() && !Core::State::GetInstance().ImeDisabled() &&
        !Hooks::PrismaBridge::OwnsInput() && GetForegroundWindow() == m_hWndParent)
    {
        logger::info("Reclaiming IME focus stolen by a click during a Meridian session");
        SetFocus(hWnd);
    }
}

void ImeWnd::HandleKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (wParam == VK_SHIFT)
    {
        m_shiftTapArmed      = true;
        m_shiftTapDownTickMs = GetTickCount64();
    }
    else
    {
        m_shiftTapArmed = false; // Shift+key combo is not a mode toggle
    }
    ForwardEditingKeyToPrismaHostIfOwned(m_hWndParent, uMsg, wParam, lParam);
}

void ImeWnd::HandleKeyUp(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (wParam == VK_SHIFT && m_shiftTapArmed)
    {
        m_shiftTapArmed = false;
        // Bare Shift tap (no other key down/up in between): for
        // Chinese IMEs this is the 中/英 toggle. Only predict while
        // the IME actually owns the keyboard and no composition is
        // active (Shift during a composition commits raw text in
        // most IMEs instead of toggling). Behavioral inference
        // corrects the display on the next keystroke if the active
        // IME does not use Shift as its toggle.
        auto       &state      = State::GetInstance();
        const bool  imeOwnsKeys = ImeController::GetInstance()->IsModEnabled() && state.NotHas(State::IME_DISABLED) &&
                                  state.Has(State::INPUT_PROCESSOR_ACTIVATED) && state.TsfFocus() &&
                                  state.NotHas(State::IN_COMPOSING, State::IN_CAND_CHOOSING);
        if (imeOwnsKeys && GetTickCount64() - m_shiftTapDownTickMs <= SHIFT_TAP_WINDOW_MS)
        {
            logger::info("Bare Shift tap while the IME owns input: toggling the displayed 中/英 state");
            if (state.GetConversionMode().IsNative())
            {
                state.ClearConversionModeFlag(State::ConversionMode::Flags::NATIVE);
            }
            else
            {
                state.AddConversionModeFlag(State::ConversionMode::Flags::NATIVE);
            }
        }
    }
    else if (wParam != VK_SHIFT)
    {
        m_shiftTapArmed = false;
    }
    ForwardEditingKeyToPrismaHostIfOwned(m_hWndParent, uMsg, wParam, lParam);
}

auto ImeWnd::HandleCharMessage(WPARAM wParam, LPARAM lParam) -> LRESULT
{
    const auto &state = Core::State::GetInstance();
    if (!(ImeController::GetInstance()->IsModEnabled() && (state.NotHas(State::IME_DISABLED) && state.Has(State::INPUT_PROCESSOR_ACTIVATED))))
    {
        logger::debug(
            "ImeWnd WM_CHAR {:#x} dropped by gate (modEnabled={}, imeDisabled={}, tipActive={})",
            static_cast<std::uint32_t>(wParam),
            ImeController::GetInstance()->IsModEnabled(),
            state.ImeDisabled(),
            state.Has(State::INPUT_PROCESSOR_ACTIVATED));
        return 0;
    }
    const auto wcharCode = static_cast<std::uint32_t>(wParam);

    // Composition echo suppression: the plain PeekMessage loop runs
    // TranslateMessage BEFORE the IME consumes the key in
    // DefWindowProc, so while a composition is active every pinyin key
    // — and the commit space — still produces a WM_CHAR here. Scaleform
    // menus mask those echoes behind ImeMenu::OnCharEvent's
    // interception, but a Meridian session routes them straight into
    // the DOM field: the raw pinyin (plus a stray space) landed next to
    // the committed text. Characters emitted during an active
    // composition belong to the IME; drop them, the composition's own
    // OnEndComposition callback delivers the committed string.
    if (State::GetInstance().HasAny(State::IN_COMPOSING, State::IN_CAND_CHOOSING))
    {
        // The IME consumed the key at the message level (these echoes
        // never even carry the composition's keys — the TIP eats them
        // upstream of this WndProc), but if one ever slips through,
        // forwarding it would double-type it into Meridian's DOM
        // field. The host's own leak of the composition keystrokes is
        // compensated in the DOM by the bridge's strip pass.
        logger::debug("ImeWnd WM_CHAR {:#x} dropped (composition active)", wParam);
        return 0;
    }

    // Commit-keystroke echo (see InitializeTextService): the echo is
    // deterministically the first WM_CHAR on this thread after the
    // commit callback ran. Drop it once, then disarm — a later
    // keystroke inside the window belongs to the user, not the echo.
    if (m_lastCommitTickMs != 0)
    {
        const auto nowTick = GetTickCount64();
        if (nowTick - m_lastCommitTickMs <= COMMIT_ECHO_DROP_WINDOW_MS)
        {
            m_lastCommitTickMs = 0;
            logger::debug("ImeWnd WM_CHAR {:#x} dropped (commit keystroke echo)", wParam);
            return 0;
        }
        m_lastCommitTickMs = 0; // window expired: nothing left to suppress
    }

    // Behavioral mode inference: an ASCII letter reaching this point was
    // passed through by the TIP with no composition involved. In native
    // (Chinese/Japanese) mode the IME consumes letters upstream — they
    // never produce a WM_CHAR here. IMEs that publish no mode
    // compartment (WeChat IME) are still tracked this way.
    if ((wcharCode >= L'a' && wcharCode <= L'z') || (wcharCode >= L'A' && wcharCode <= L'Z'))
    {
        logger::debug("ImeWnd WM_CHAR {:c} passed through raw — clearing NATIVE conversion mode", wParam);
        State::GetInstance().ClearConversionModeFlag(State::ConversionMode::Flags::NATIVE);
    }

    // The direct keys(arrow keys, etc.) are not sent via WM_CHAR messages
    static const auto ignoredKeys = {VK_TAB, VK_RETURN, VK_BACK, VK_ESCAPE /*, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT*/};
    if (std::ranges::find(ignoredKeys, wcharCode) == std::end(ignoredKeys))
    {
        logger::debug("ImeWnd WM_CHAR {:#x} accepted, forwarding to Skyrim", wcharCode);
        const std::wstring wstring(1, LOWORD(wParam));
        Skyrim::SendUiString(wstring);
    }
    return 0;
}

auto ImeWnd::OnNccCreate(HWND hWnd, LPCREATESTRUCT lpCreateStruct) -> LRESULT
{
    auto *pThis = static_cast<ImeWnd *>(lpCreateStruct->lpCreateParams);
    SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    pThis->m_hWnd       = hWnd;
    pThis->m_hWndParent = lpCreateStruct->hwndParent;
    return TRUE;
}

void ImeWnd::OnCreated(Settings &settings)
{
    logger::info("Ime window created, init TSF and core...");
    m_gameThreadId = GetWindowThreadProcessId(m_hWndParent, nullptr);
    // The Prisma commit route posts WM_CHAR at the game window (ImeWnd's
    // parent): PrismaUI's subclass feeds those into the focused view.
    Hooks::PrismaBridge::SetGameHwnd(m_hWndParent);
    m_textService->OnStart(m_hWnd);
    m_uiScale            = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hWnd);
    m_fWantUpdateUiScale = true;
    float uiScale        = settings.appearance.zoom;
    uiScale              = static_cast<float>(align_to(static_cast<int>(uiScale * 100.0F), Settings::ZOOM_STEP_PERCENT)) / 100.F;
    if (uiScale > 0.0F)
    {
        m_uiScale = std::clamp(uiScale, Settings::ZOOM_MIN, Settings::ZOOM_MAX);
    }

    ImeController::GetInstance()->Init(this, m_hWndParent, settings);
}

auto ImeWnd::OnDestroy() -> LRESULT
{
    logger::info("Destroy IME Window");
    // One-shot teardown latch: WM_DESTROY must tear down exactly once. The
    // exchange makes a second entry (e.g. a late DestroyWindow after
    // ImeApp::Shutdown's timeout path already assumed the worst) skip straight
    // to PostQuitMessage instead of double-running the TSF/COM/ImGui teardown,
    // which the game thread's Uninitialize may meanwhile be racing.
    if (m_fTearingDown.exchange(true))
    {
        PostQuitMessage(0);
        return S_OK;
    }
    // Drain queued IME-thread tasks BEFORE tearing the controller down: this is
    // the same thread the tasks execute on, and ImeController/m_imeWnd/m_delegate
    // are still alive here. Running them after Shutdown would dereference the
    // nulled members (the tasks' IsReady() re-check is only a backstop).
    TaskQueue::GetInstance().ExecuteImeThreadTasks();
    ImeController::GetInstance()->Shutdown();
    UnInitialize();
    PostQuitMessage(0);
    return S_OK;
}

void ImeWnd::DrawImeStates()
{
#ifdef _DEBUG
    const auto &state     = Core::State::GetInstance();
    auto        stateIcon = [](bool enable) constexpr -> void {
        if (enable)
        {
            ImGuiEx::M3::Icon(ICON_EYE, ImGuiEx::M3::Spec::SizeTips::SMALL);
        }
        else
        {
            ImGuiEx::M3::Icon(ICON_EYE_OFF, ImGuiEx::M3::Spec::SizeTips::SMALL);
        }
    };

    const auto styleGuard = ImGuiEx::StyleGuard().Style<ImGuiStyleVar_WindowPadding>(ImVec2(20.0F, 20.0F));
    if (!ImGui::Begin("SimpleIme Debug", nullptr, ImGuiEx::WindowFlags().AlwaysAutoResize()))
    {
        ImGui::End();
        return;
    }
    ImGui::Value("IME focused", m_fFocused);
    bool textServiceFocused = state.Has(Core::State::TEXT_SERVICE_FOCUS);
    if (ImGui::Checkbox("Toggle TSF Focus", &textServiceFocused))
    {
        if (!m_textService->OnFocus(textServiceFocused))
        {
            ErrorNotifier::GetInstance().Debug("Failed to toggle text service focus.");
        }
    }

    // clang-format off
    const auto & conversionMode = state.GetConversionMode();
    // INPUT_PROCESSOR_ACTIVATED means "any known profile is active" (it gates
    // text forwarding), so the light must additionally check the profile type:
    // only a real TIP (input processor) is an IME — a plain keyboard layout
    // (e.g. the English keyboard switched to on disable) must leave it off.
    const bool inputProcessorActive =
        state.Has(Core::State::INPUT_PROCESSOR_ACTIVATED) && m_inputMethodManager != nullptr &&
        m_inputMethodManager->GetActiveLangProfile().dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR;
    struct DebugFlag
    {
        bool        on;
        const char *label;
    };
    // Chips flow left to right inside one group; a new group starts a new line
    // (SameLine only between chips — a trailing one would glue the groups).
    const auto drawFlags = [&stateIcon](const std::initializer_list<DebugFlag> &flags) {
        bool first = true;
        for (const auto &flag : flags)
        {
            if (!first)
            {
                ImGui::SameLine();
            }
            first = false;
            stateIcon(flag.on);
            ImGui::SameLine();
            ImGuiEx::M3::AlignedLabel(flag.label);
        }
    };
    drawFlags({
        {textServiceFocused,                       "TEXT_SERVICE_FOCUS"},
        {state.Has(Core::State::IN_COMPOSING),     "IN_COMPOSING"},
        {state.Has(Core::State::IN_CAND_CHOOSING), "IN_CAND_CHOOSING"},
        {conversionMode.IsAlphanumeric(),          "CMode: ALPHANUMERIC"},
        {conversionMode.IsNative(),                "CMode: NATIVE"},
        {conversionMode.IsKatakana(),              "CMode: KATAKANA"},
        {conversionMode.IsFullShape(),             "CMode: FULLSHAPE"},
        {conversionMode.IsRoman(),                 "CMode: ROMAN"},
        {conversionMode.IsCharCode(),              "CMode: CHARCODE"},
        {conversionMode.IsSoftKeyboard(),          "CMode: SOFTKEYBOARD"},
        {conversionMode.IsNoConversion(),          "CMode: NOCONVERSION"},
        {conversionMode.IsEudc(),                  "CMode: EUDC"},
        {conversionMode.IsSymbol(),                "CMode: SYMBOL"},
        {conversionMode.IsFixed(),                 "CMode: FIXED"},
    });
    drawFlags({
        {inputProcessorActive,               "INPUT_PROCESSOR_ACTIVATED"},
        {state.Has(Core::State::IME_DISABLED), "IME_DISABLED"},
        {state.Has(Core::State::GAME_LOADING), "GAME_LOADING"},
        {state.Has(Core::State::KEYBOARD_OPEN), "KEYBOARD_OPEN"},
    });
    // clang-format on
    ImGui::End();
#endif
}
} // namespace Ime
