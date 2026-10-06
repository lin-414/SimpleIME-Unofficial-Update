//
// SKSE Menu Framework text-input bridge.
//
// SKSEMF-based mod UIs draw their settings windows with ImGui embedded in
// SKSEMenuFramework.dll. Their InputText fields never touch the Scaleform menu
// stack, and focusing one does not raise the game's text-entry counter — so
// SimpleIME's stock activation and GFx char-event delivery both miss them.
//
// This bridge closes both gaps through the framework's own exports:
//  1. A render-event callback (kBeforeRender) polls ImGui's WantTextInput.
//     On a false->true transition it acquires a "text-entry lease" — one call
//     to the native ControlMap::AllowTextInput, which flows through our own
//     detour and turns the IME on exactly like a vanilla text entry. The lease
//     is released when WantTextInput drops, so the counter never leaks.
//  2. Committed IME text is queued here (IME thread) and injected with
//     ImGuiIO_AddInputCharacter from inside the framework callback — the same
//     thread its ImGui frames run on, so the input queue is never raced.
//  3. While a session is active, an input-dispatch hook neutralizes the
//     printable-ASCII CharEvents the engine keeps generating from DirectInput
//     during composition (DirectInput polls the hardware regardless of Win32
//     focus); in English mode they pass through untouched, so plain typing
//     still works. The session/predicate gates keep the hook inert everywhere
//     else.
//
// The export surface, the WantTextInput offset and the callback technique are
// adapted from cashboxs/TMS_SIMEtoSKSEMF (MIT), see THIRD-PARTY-NOTICES.md.
//
#include "hooks/SkseMenuFrameworkBridge.h"

#include "ImeApp.h"
#include "RE/ControlMap.h"
#include "core/State.h"
#include "hook.h"
#include "hooks/SkseMenuFrameworkBridgeLogic.h"
#include "log.h"

#include <REL/REL.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace Hooks::SkseMenuFrameworkBridge
{
namespace
{
// The framework gate matches TMS_SIMEtoSKSEMF: RegisterEventPriority and the
// cimgui text exports appeared in 3.7.
constexpr float MIN_FRAMEWORK_VERSION = 3.7F;
/// Event::EventType::kBeforeRender in SKSE-Menu-Framework 3 (include/Event.h).
constexpr std::int32_t EVENT_BEFORE_RENDER = 3;
/// Run before the framework's own render listeners, exactly like the
/// standalone bridge does — proven to see a fresh WantTextInput and to inject
/// into a queue the same frame's NewFrame still drains.
constexpr float EVENT_PRIORITY = -1000.0F;

/// Offset of ImGuiIO::WantTextInput inside the ImGui build embedded in
/// SKSEMenuFramework.dll. There is no exported field getter, so the offset is
/// pinned against the RELEASED framework DLL (3.8, the one this feature
/// targets) by two independent measurements:
///   * Disassembly of its exported ImGuiIO_AddInputCharacter / _UTF16: they
///     touch io->Ctx @ 0xF0, io->AppAcceptingEvents @ 0x2BB9 and
///     io->InputQueueSurrogate @ 0x2BBC.
///   * offsetof probes against the framework's own amalgamated header
///     (ImGuiMCP::ImGuiIO, the mirror the standalone TMS bridge compiles
///     against): Ctx=0xF0, AppAcceptingEvents=0xBB9, Surrogate=0xBBC — the
///     released DLL builds its KeysData[] as [ImGuiKey_COUNT] instead of
///     [ImGuiKey_NamedKey_COUNT], which shifts everything *after* KeysData by
///     exactly 0x2000 and nothing before it (Ctx sits before, and matches).
/// WantTextInput precedes KeysData, so the pre-shift value applies. If a
/// future framework rebuild drifts, the symptom is a silent bridge (no
/// session-begin log); re-anchor on the user's DLL the same way.
constexpr std::size_t IMGUI_IO_WANT_TEXT_INPUT_OFFSET = 0xCC;
/// ImGuiIO::ConfigDebugIgnoreFocusLoss (official since 1.89, pre-KeysData so
/// the offset is not shifted by the DLL's KeysData layout): makes
/// AddFocusEvent(false) a no-op. Set on the framework's io because SimpleIME
/// MUST move the Win32 focus to its own ImeWnd while composing (TSF lives on
/// the IME thread) — the game window then receives WM_KILLFOCUS, and the
/// framework's ImGui cleared its input state on it, deactivating the focused
/// InputText two frames into every session when the menu's freeze-time pause
/// was on (observed 2026-10-04: language bar flashing off). An embedded game
/// overlay loses nothing real by ignoring host focus loss: while the game
/// window is truly unfocused (alt-tab) the game stops rendering the framework
/// anyway.
constexpr std::size_t IMGUI_IO_CONFIG_IGNORE_FOCUS_LOSS_OFFSET = 0x7B;

/// A session whose framework render loop has been silent this long is dead
/// (its menu closed without an ImGui frame ever reporting the field's
/// deactivation) — release the lease before the leak healer has to.
/// 3s, not lower: real games hitch for hundreds of ms to seconds while a
/// framework menu is open (asset streaming, our own first-CJK font atlas
/// build) — kBeforeRender stops for exactly that long and the field is still
/// live. At 600ms the watchdog killed such live sessions mid-composition
/// (observed 2026-10-05: force-end at 27s into a session, lease released,
/// IME disabled, bar gone; the 1ms-later re-begin was debounce-deferred and
/// the deferred enable was dropped, dead session until re-click), and hitches
/// past 3s still recover: the re-begin's enable is now committed by
/// CommitPendingTextEntryEnable instead of dropped.
constexpr std::uint64_t STALL_TIMEOUT_MS = 3000;
/// Committed UTF-16 units accepted while waiting for a flush (paste flood cap,
/// same bound as the Meridian queue).
constexpr std::size_t MAX_PENDING_UNITS = 8192;

/// lazily-resolved SKSEMenuFramework.dll export surface.
struct FrameworkApi
{
    using EventCallback = void (__stdcall *)(std::int32_t);
    using RegisterEventPriorityFn = std::int64_t (*)(EventCallback, float);
    using GetVersionFn = float (*)();
    using GetCurrentContextFn = void *(*)();
    using GetIoFn = void *(*)();
    using GetFrameCountFn = int (*)();
    using AddInputCharacterFn = void (*)(void *, unsigned int);

    HMODULE                 module = nullptr;
    RegisterEventPriorityFn registerEventPriority = nullptr;
    GetVersionFn            getVersion = nullptr;
    GetCurrentContextFn     getCurrentContext = nullptr;
    GetIoFn                 getIo = nullptr;
    GetFrameCountFn         getFrameCount = nullptr;
    AddInputCharacterFn     addInputCharacter = nullptr;

    template <class T>
    bool ResolveOne(T &target, const char *name) const
    {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        if (target == nullptr)
        {
            logger::error("SKSE Menu Framework export missing: {}", name);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool Resolve()
    {
        module = GetModuleHandleW(L"SKSEMenuFramework.dll");
        if (module == nullptr)
        {
            return false;
        }
        bool ok = true;
        ok = ResolveOne(registerEventPriority, "RegisterEventPriority") && ok;
        ok = ResolveOne(getVersion, "GetMenuFrameworkVersion") && ok;
        ok = ResolveOne(getCurrentContext, "igGetCurrentContext") && ok;
        ok = ResolveOne(getIo, "igGetIO") && ok;
        ok = ResolveOne(getFrameCount, "igGetFrameCount") && ok;
        ok = ResolveOne(addInputCharacter, "ImGuiIO_AddInputCharacter") && ok;
        return ok;
    }
};

// ---- state ------------------------------------------------------------
// s_api is resolved on the main thread (Install) or the game thread (Tick
// retry) and only read after s_apiReady is observed; the framework callback
// runs on the framework's render thread and only touches the io via exports.
// Session flags are atomics shared with the IME thread (ShouldRoute) and the
// input-dispatch hook (NeutralizeRawAscii). The pending-text queue is the only
// mutex-guarded handoff (IME thread produces, framework callback consumes).
FrameworkApi                   s_api;
std::atomic<bool>              s_enabled{false};       ///< config gate, latched at Install
std::atomic<SupportState>      s_state{SupportState::Pending}; ///< install outcome, for the settings UI
std::atomic<bool>              s_apiReady{false};
std::atomic<bool>              s_sessionActive{false};
std::atomic<bool>              s_leaseHeld{false};
std::atomic<std::uint64_t>     s_lastFrameworkEventMs{0};
std::atomic<bool>              s_hookInstalled{false};
std::mutex                     s_pendingMutex;
std::deque<char16_t>           s_pending;
std::unique_ptr<DispatchInputEventHookData> s_inputHook; ///< never uninstalled; passthrough while no session

// ---- native text-entry lease ------------------------------------------
// IMPORTANT: go through the engine's AllowTextInput (the address our own
// detour sits on), NOT Ime::ControlMap::SKSE_AllowTextInput — the latter pokes
// the counter member directly and would never fire OnTextEntryCountChanged,
// leaving the IME (and the language bar) off while the counter says otherwise.
using AllowTextInputFn = std::uint8_t(Ime::ControlMap *, bool);

std::uint8_t CallNativeAllowTextInput(bool allow)
{
    auto *controlMap = Ime::ControlMap::GetSingleton();
    if (controlMap == nullptr)
    {
        return 0;
    }
    static REL::Relocation<AllowTextInputFn> native{REL::RelocationID(67252, 68552)};
    return native(controlMap, allow);
}

void AcquireTextEntryLease()
{
    if (s_leaseHeld.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    CallNativeAllowTextInput(true);
}

void ReleaseTextEntryLease()
{
    if (!s_leaseHeld.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    CallNativeAllowTextInput(false);
}

// ---- session bookkeeping (framework callback thread) -------------------
// Fresh-enable transition: when a session begins while the IME is still
// disabled (the common first-click case — the lease triggers the async enable
// ~20ms later), the mode flags (IME_DISABLED / KEYBOARD_OPEN / NATIVE) are
// stale for a few more frames. Keystroke echoes inside this window cannot be
// classified by the mode predicate and would leak a leading letter into the
// field (observed: "d但是"), so NeutralizeRawAscii neutralizes them for the
// bounded TRANSITION_GRACE_MS after session begin. Sessions that start with
// the IME already up (keepImeOpen users, re-entering a field) get no window.
constexpr std::uint64_t TRANSITION_GRACE_MS = 250;

std::atomic<std::uint64_t> s_sessionBeginMs{0};
std::atomic<bool>          s_imeDisabledAtBegin{false};

void BeginTextInput()
{
    AcquireTextEntryLease();
    s_sessionBeginMs.store(GetTickCount64(), std::memory_order_release);
    s_imeDisabledAtBegin.store(Ime::Core::State::GetInstance().ImeDisabled(), std::memory_order_release);
    logger::info("SKSE Menu Framework text session begin (lease acquired)");
}

void EndTextInput()
{
    ReleaseTextEntryLease();
    logger::info("SKSE Menu Framework text session end (lease released)");
}

void UpdateTextInputState()
{
    if (s_api.getCurrentContext == nullptr || s_api.getIo == nullptr || s_api.getCurrentContext() == nullptr)
    {
        return;
    }
    auto *io = s_api.getIo();
    if (io == nullptr)
    {
        return;
    }
    const bool wants = *reinterpret_cast<const volatile bool *>(reinterpret_cast<const std::uint8_t *>(io) + IMGUI_IO_WANT_TEXT_INPUT_OFFSET);
    const bool previous = s_sessionActive.exchange(wants, std::memory_order_acq_rel);
    if (wants == previous)
    {
        return;
    }
    if (wants)
    {
        BeginTextInput();
    }
    else
    {
        EndTextInput();
    }
}

// ---- committed text injection (framework callback thread) --------------
void InjectCodepoint(unsigned int codepoint)
{
    // Rejects ASCII (the raw CharEvents already deliver English typing) and
    // surrogates (recombined upstream in FlushPending) — see the logic header.
    if (!SkseMenuFrameworkBridgeLogic::IsValidUnicodeScalar(codepoint))
    {
        return;
    }
    if (s_api.getCurrentContext == nullptr || s_api.getCurrentContext() == nullptr || s_api.getIo == nullptr ||
        s_api.addInputCharacter == nullptr)
    {
        return;
    }
    auto *io = s_api.getIo();
    if (io == nullptr)
    {
        return;
    }
    s_api.addInputCharacter(io, codepoint);
}

/// UTF-16 units -> codepoints (surrogate pairs recombined; astral characters
/// arrive from composition strings and Unicode paste as pairs, while ImGui
/// wants whole codepoints). An unpaired high surrogate is kept queued for the
/// next flush; anything else invalid is dropped.
void FlushPending()
{
    std::deque<char16_t> batch;
    {
        const std::scoped_lock lock(s_pendingMutex);
        batch.swap(s_pending);
    }
    if (batch.empty())
    {
        return;
    }
    if (!s_sessionActive.load(std::memory_order_acquire))
    {
        // The field deactivated before the commit landed — the queue is stale.
        logger::debug("Dropped {} stale committed unit(s) after session end", batch.size());
        return;
    }

    char16_t highSurrogate = 0;
    for (const char16_t unit : batch)
    {
        if (highSurrogate != 0)
        {
            if (unit >= 0xDC00 && unit <= 0xDFFF)
            {
                const auto codepoint = 0x10000U + ((static_cast<std::uint32_t>(highSurrogate) - 0xD800U) << 10) +
                                       (static_cast<std::uint32_t>(unit) - 0xDC00U);
                InjectCodepoint(codepoint);
                highSurrogate = 0;
                continue;
            }
            logger::debug("Dropped unpaired high surrogate {:04x}", static_cast<unsigned>(highSurrogate));
            highSurrogate = 0;
            // Fall through: `unit` is processed as a fresh unit below.
        }
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            highSurrogate = unit;
        }
        else if (unit >= 0xDC00 && unit <= 0xDFFF)
        {
            logger::debug("Dropped unpaired low surrogate {:04x}", static_cast<unsigned>(unit));
        }
        else
        {
            InjectCodepoint(unit);
        }
    }
    if (highSurrogate != 0)
    {
        const std::scoped_lock lock(s_pendingMutex);
        s_pending.push_front(highSurrogate); // wait for its low half next flush
    }
}

// ---- framework render-event callback -----------------------------------
void __stdcall OnFrameworkEvent(std::int32_t eventType)
{
    if (!s_enabled.load(std::memory_order_acquire))
    {
        return;
    }
    s_lastFrameworkEventMs.store(GetTickCount64(), std::memory_order_release);

    // One-time per-event-type discovery: kOpenMenu/kCloseMenu (2/1) prove the
    // user actually opened a framework window; kAfterRender (4) proves the
    // full frame loop. Missing lines are as informative as present ones.
    if (eventType >= 0 && eventType <= 4)
    {
        static std::atomic<bool> s_seenType[5]{};
        if (!s_seenType[eventType].exchange(true))
        {
            logger::info("SKSE Menu Framework event type {} first seen", eventType);
        }
    }
    if (eventType != EVENT_BEFORE_RENDER)
    {
        return;
    }

    // Throttled io-flag telemetry (only on change or every 5s): the three
    // Want* bools say whether ImGui is even seeing the user's mouse/keyboard
    // and whether a text field is active. With no session ever beginning,
    // One-time, on the first framework frame: stop the framework's ImGui from
    // wiping its input state when our IME enable moves the Win32 focus to
    // ImeWnd (see the offset constant above for the full story — this was the
    // freeze-time-on "language bar flashes off" bug, fixed 2026-10-04).
    static std::atomic<bool> s_ignoreFocusLossSet{false};
    if (!s_ignoreFocusLossSet.exchange(true, std::memory_order_acq_rel) && s_api.getCurrentContext != nullptr &&
        s_api.getIo != nullptr && s_api.getCurrentContext() != nullptr)
    {
        if (auto *io = s_api.getIo(); io != nullptr)
        {
            reinterpret_cast<std::uint8_t *>(io)[IMGUI_IO_CONFIG_IGNORE_FOCUS_LOSS_OFFSET] = 1;
            logger::info("SKSE Menu Framework io: ConfigDebugIgnoreFocusLoss enabled (our IME focus moves must not clear its input state)");
        }
    }

    // Flush BEFORE the transition check: the final commit of a session races
    // the field's deactivation, and injecting one frame late is harmless
    // (ImGui discards unconsumed input) while dropping it eats the last char.
    FlushPending();
    UpdateTextInputState();
}

// ---- input-dispatch thunk (ASCII neutralization) -----------------------
void DispatchInputEventThunk(RE::BSTEventSource<RE::InputEvent *> *dispatcher, RE::InputEvent **events)
{
    NeutralizeRawAscii(events);
    s_inputHook->Original(dispatcher, events);
}

// ---- detection ----------------------------------------------------------
std::atomic<bool> s_resolveInFlight{false};
std::atomic<std::uint64_t> s_lastResolveAttemptMs{0};
constexpr std::uint64_t RESOLVE_RETRY_MS = 5000;

bool TryResolve()
{
    if (s_apiReady.load(std::memory_order_acquire))
    {
        return true;
    }
    if (s_resolveInFlight.exchange(true, std::memory_order_acq_rel))
    {
        return false;
    }

    const auto now      = GetTickCount64();
    bool       eligible = s_lastFrameworkEventMs.load(std::memory_order_relaxed) != 0 ||
                    (now - s_lastResolveAttemptMs.load(std::memory_order_relaxed)) >= RESOLVE_RETRY_MS;
    if (!eligible)
    {
        s_resolveInFlight.store(false, std::memory_order_release);
        return false;
    }
    s_lastResolveAttemptMs.store(now, std::memory_order_relaxed);

    if (!s_api.Resolve())
    {
        s_state = SupportState::NotDetected;
        logger::info("SKSEMenuFramework.dll not loaded (yet); SKSE Menu Framework input support stays off");
        s_resolveInFlight.store(false, std::memory_order_release);
        return false;
    }

    const float version = s_api.getVersion();
    if (version + 0.0001F < MIN_FRAMEWORK_VERSION)
    {
        s_state = SupportState::Failed;
        logger::warn("SKSE Menu Framework {:.2f} is too old; {:.2f}+ required for IME input support", version, MIN_FRAMEWORK_VERSION);
        s_resolveInFlight.store(false, std::memory_order_release);
        return false;
    }

    s_api.registerEventPriority(&OnFrameworkEvent, EVENT_PRIORITY);

    // Install the raw-ASCII filter only when the framework is actually present
    // (the detour is inert outside a session, but there is no reason to take
    // it for users without the framework at all). Same single-threaded-install
    // invariant as the other HookData users: this runs on the main thread at
    // kDataLoaded (or the game thread in Tick) before a session can exist.
    if (!s_hookInstalled.exchange(true, std::memory_order_acq_rel))
    {
        s_inputHook = std::make_unique<DispatchInputEventHookData>(&DispatchInputEventThunk);
    }

    s_apiReady.store(true, std::memory_order_release);
    s_state = SupportState::Active;
    logger::info("SKSE Menu Framework bridge ready (framework {:.2f}, input filter installed)", version);
    s_resolveInFlight.store(false, std::memory_order_release);
    return true;
}
} // namespace

void Install()
{
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;

    s_enabled = Ime::ImeApp::GetInstance().GetSettings().input.skseMenuFrameworkSupport;
    if (!s_enabled.load())
    {
        s_state = SupportState::Off;
        logger::info("SKSE Menu Framework input support disabled by configuration");
        return;
    }

    // Mutual exclusion with the standalone TMS_SIMEtoSKSEMF bridge: it performs
    // the very same lease + injection dance, so both active at once would
    // double-inject every commit (same standoff semantics as
    // MeridianBridge::Install's rival-plugin check).
    if (GetModuleHandleW(L"TMS_SIMEtoSKSEMF.dll") != nullptr)
    {
        s_state = SupportState::Standoff;
        logger::warn("TMS_SIMEtoSKSEMF detected; SimpleIME's built-in SKSE Menu Framework input support stays off to avoid double text injection");
        return;
    }

    (void)TryResolve();
}

SupportState State()
{
    return s_state.load(std::memory_order_acquire);
}

void Uninstall()
{
    ForceEndSession();
    s_enabled = false;
    s_apiReady = false;
}

void Tick()
{
    if (!s_enabled.load(std::memory_order_acquire))
    {
        return;
    }
    if (!s_apiReady.load(std::memory_order_acquire))
    {
        (void)TryResolve();
        return;
    }
    if (!s_sessionActive.load(std::memory_order_acquire))
    {
        return;
    }
    // The framework stops rendering (and dispatching kBeforeRender) once its
    // last window closes; if a text field was focused at that moment its
    // WantTextInput stays stale forever. Treat a silent render loop as the
    // session's end so the lease cannot outlive its owner.
    const auto last = s_lastFrameworkEventMs.load(std::memory_order_acquire);
    if (last != 0 && GetTickCount64() - last > STALL_TIMEOUT_MS)
    {
        logger::warn("SKSE Menu Framework render loop went silent during a text session, forcing session end");
        ForceEndSession();
    }
}

bool SessionActive()
{
    return s_enabled.load(std::memory_order_acquire) && s_sessionActive.load(std::memory_order_acquire);
}

bool ShouldRoute()
{
    return SessionActive();
}

void QueueText(std::wstring_view text)
{
    if (text.empty())
    {
        return;
    }
    const std::scoped_lock lock(s_pendingMutex);
    for (const wchar_t c : text)
    {
        // Same strip list as the Scaleform commit path: grave would toggle the
        // console when echoed back, and the middle dot is the CJK list
        // separator the engine treats as a hotkey.
        constexpr wchar_t GRAVE_ACCENT = L'`';
        constexpr wchar_t MIDDLE_DOT   = L'·';
        if (c == GRAVE_ACCENT || c == MIDDLE_DOT)
        {
            continue;
        }
        if (s_pending.size() >= MAX_PENDING_UNITS)
        {
            logger::warn("Committed-text queue overflow, dropping the rest of the chunk");
            break;
        }
        s_pending.push_back(static_cast<char16_t>(c));
    }
}

void OnTextEntryCountHealed()
{
    if (s_leaseHeld.exchange(false, std::memory_order_acq_rel))
    {
        logger::info("Text-entry lease cleared by the leak healer; SKSEMF session reset");
        ForceEndSession();
    }
}

void ForceEndSession()
{
    const bool wasActive = s_sessionActive.exchange(false, std::memory_order_acq_rel);
    if (wasActive)
    {
        logger::info("SKSE Menu Framework text session force-ended");
    }
    ReleaseTextEntryLease();
    const std::scoped_lock lock(s_pendingMutex);
    if (!s_pending.empty())
    {
        logger::debug("Discarded {} queued committed unit(s) on session end", s_pending.size());
        s_pending.clear();
    }
}

void NeutralizeRawAscii(RE::InputEvent *const *events)
{
    if (!s_enabled.load(std::memory_order_acquire) || !s_sessionActive.load(std::memory_order_acquire) || events == nullptr ||
        *events == nullptr)
    {
        return;
    }

    const auto &state = Ime::Core::State::GetInstance();
    const bool freshEnableTransition =
        s_imeDisabledAtBegin.load(std::memory_order_acquire) &&
        (GetTickCount64() - s_sessionBeginMs.load(std::memory_order_acquire)) < TRANSITION_GRACE_MS;
    const bool tipActive = state.Has(Ime::Core::State::INPUT_PROCESSOR_ACTIVATED);
    if (!SkseMenuFrameworkBridgeLogic::ShouldNeutralizeAscii(
            /*sessionActive=*/true,
            state.IsImeInputting(),
            state.ImeDisabled(),
            state.IsKeyboardOpen(),
            state.GetConversionMode().IsNative(),
            freshEnableTransition,
            tipActive))
    {
        return;
    }

    std::uint32_t neutralized = 0;
    for (auto *event = *events; event != nullptr; event = event->next)
    {
        if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kChar)
        {
            continue;
        }
        auto *charEvent = event->AsCharEvent();
        if (charEvent == nullptr)
        {
            continue;
        }
        const auto codepoint = static_cast<std::uint32_t>(charEvent->keyCode);
        if (!SkseMenuFrameworkBridgeLogic::IsPrintableAscii(codepoint))
        {
            continue;
        }
        charEvent->keyCode = 0;
        ++neutralized;
    }
    if (neutralized != 0)
    {
        logger::debug("Neutralized {} composition ASCII char event(s) for the SKSEMF session", neutralized);
    }
}
} // namespace Hooks::SkseMenuFrameworkBridge
