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
#include "RE/B/ButtonEvent.h"
#include "RE/C/CursorMenu.h"
#include "RE/ControlMap.h"
#include "RE/I/InputDevices.h"
#include "RE/M/MenuCursor.h"
#include "REX/W32/DINPUT.h"
#include "core/State.h"
#include "hook.h"
#include "hooks/BridgeGate.h"
#include "hooks/SkseMenuFrameworkBridgeLogic.h"
#include "hooks/ScopeFlag.h"
#include "log.h"
#include "path_utils.h"
#include "utils/Utils.h"

#include <REL/REL.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
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
/// Empirical from the first-seen telemetry: type 4 dispatches after the
/// framework's full frame loop, i.e. when ImGui's InputText has just written
/// this frame's OS-IME caret into PlatformImeData (kBeforeRender reads it
/// mid-frame where NewFrame may already have reset WantVisible to false).
constexpr std::int32_t EVENT_AFTER_RENDER = 4;
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

/// Layout mirror of ImGuiPlatformImeData (1.90.8: bool + ImVec2 + float).
struct ImGuiPlatformImeDataMirror
{
    std::uint8_t wantVisible;
    float        inputPosX;
    float        inputPosY;
    float        inputLineHeight;
};
static_assert(sizeof(ImGuiPlatformImeDataMirror) == 16);

// Candidate-window anchor shared with ImeWindow (game thread reads it). The
// SetPlatformImeDataFn hook maintains it; the click-time heuristic in
// BeginTextInput is the fallback when no live source could be installed.
std::atomic<bool>  s_fieldAnchorValid{false};
std::atomic<float> s_fieldAnchorX{0.0F};
std::atomic<float> s_fieldAnchorY{0.0F};
std::atomic<bool>  s_imeDataPathBroken{false}; ///< no live anchor source could be installed

bool ImeDataPatternPlausible(const ImGuiPlatformImeDataMirror *ime);

/// --- PlatformImeData via io.SetPlatformImeDataFn (primary anchor source) ---
/// ImGuiIO's layout in this framework is NOT vanilla 1.90.8: the runtime-proven
/// offsets sit 8 bytes past the vanilla ones (WantTextInput 0xCC vs 196,
/// ConfigDebugIgnoreFocusLoss 0x7B vs 115), so every vanilla-derived context
/// offset misses. The fn-pointer route sidesteps layout knowledge entirely:
/// ImGui hands &g.PlatformImeData to io.SetPlatformImeDataFn whenever the data
/// changes (imgui.cpp EndFrame, fires on field activation / caret move /
/// deactivation), and that pointer is real regardless of fork shifts.
/// Vanilla offsetof(ImGuiIO, SetPlatformImeDataFn)=184 sits between the two
/// proven anchors, so the framework slot is at 184+8=192; installing is gated
/// on the slot holding a pointer INTO the framework module (its embedded
/// imgui_impl_win32 sets it), which proves the offset before anything is
/// overwritten. If the gate fails, the runtime calibration below takes over.
constexpr std::size_t IMGUI_IO_SET_PLATFORM_IME_DATA_FN_OFFSET = 192;
using SetPlatformImeDataFn_t = void (*)(void *viewport, void *imeData);

SetPlatformImeDataFn_t s_origSetPlatformImeDataFn = nullptr;
std::atomic<bool>      s_platformImeHookInstalled{false};

void HookedSetPlatformImeData(void *viewport, void *imeData)
{
    if (imeData != nullptr)
    {
        const auto *ime = static_cast<const ImGuiPlatformImeDataMirror *>(imeData);
        // InputPos is the caret line's top-left in screen space; anchor at its
        // bottom so the candidate window hangs just below the text line.
        if (ime->wantVisible == 0 || !ImeDataPatternPlausible(ime))
        {
            // Field deactivated (or garbage): the anchor must not outlive it.
            s_fieldAnchorValid.store(false, std::memory_order_release);
        }
        else
        {
            s_fieldAnchorX.store(ime->inputPosX, std::memory_order_release);
            s_fieldAnchorY.store(ime->inputPosY + ime->inputLineHeight, std::memory_order_release);
            s_fieldAnchorValid.store(true, std::memory_order_release);
        }
    }
    if (s_origSetPlatformImeDataFn != nullptr)
    {
        s_origSetPlatformImeDataFn(viewport, imeData);
    }
}

/// The framework module handle, resolved at every use site (nothing here can
/// keep the DLL pinned against unload).
HMODULE FrameworkModule()
{
    return GetModuleHandleW(L"SKSEMenuFramework.dll");
}

/// Identity (size, last-write time) of the loaded framework DLL — the
/// fingerprint keying the anchor cache. False when the module is gone or its
/// file attributes cannot be queried.
bool TryFrameworkIdentity(std::uint64_t &a_size, std::uint64_t &a_mtime)
{
    const HMODULE module = FrameworkModule();
    wchar_t path[MAX_PATH] = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0 ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
    {
        return false;
    }
    a_size  = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    a_mtime = (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
              attributes.ftLastWriteTime.dwLowDateTime;
    return true;
}

void InstallPlatformImeDataHook(void *io)
{
    if (io == nullptr || s_platformImeHookInstalled.load(std::memory_order_acquire))
    {
        return;
    }
    const auto slot = reinterpret_cast<SetPlatformImeDataFn_t *>(
        reinterpret_cast<std::uint8_t *>(io) + IMGUI_IO_SET_PLATFORM_IME_DATA_FN_OFFSET);
    const auto original = *slot;
    HMODULE owner = nullptr;
    const HMODULE frameworkModule = FrameworkModule();
    if (original == nullptr || frameworkModule == nullptr ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(original), &owner) ||
        owner != frameworkModule)
    {
        // The slot doesn't hold a framework-module pointer: the offset guess is
        // wrong for this build. Touch nothing — the calibration path covers.
        return;
    }
    s_origSetPlatformImeDataFn = original;
    *slot = &HookedSetPlatformImeData;
    s_platformImeHookInstalled.store(true, std::memory_order_release);
    logger::info("Hooked ImGuiIO::SetPlatformImeDataFn at io+{} — field anchors are event-driven", IMGUI_IO_SET_PLATFORM_IME_DATA_FN_OFFSET);
}

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
    using IsMousePosValidFn = bool (*)(const void *);
    using AddMousePosEventFn = void (*)(void *, float, float);
    using GetActiveIdFn = unsigned int (*)();
    using GetHoveredIdFn = unsigned int (*)();

    HMODULE                 module = nullptr;
    RegisterEventPriorityFn registerEventPriority = nullptr;
    GetVersionFn            getVersion = nullptr;
    GetCurrentContextFn     getCurrentContext = nullptr;
    GetIoFn                 getIo = nullptr;
    GetFrameCountFn         getFrameCount = nullptr;
    AddInputCharacterFn     addInputCharacter = nullptr;
    /// Optional pair (both present in 3.18+): the host-mouse-position repair and
    /// the telemetry that proves it. A build without them loses that repair only,
    /// so they must not fail the whole bridge resolution.
    IsMousePosValidFn       isMousePosValid = nullptr;
    AddMousePosEventFn      addMousePosEvent = nullptr;
    /// Optional telemetry: whose focus the host's ImGui is holding. Distinguishes
    /// "the field was cleared" from "the field's window stopped being submitted"
    /// when a session ends, which is what the 2026-10-12 sessions need.
    GetActiveIdFn           getActiveId = nullptr;
    GetHoveredIdFn          getHoveredId = nullptr;

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

    template <class T>
    bool ResolveOptional(T &target, const char *name) const
    {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        return target != nullptr;
    }

    [[nodiscard]] bool Resolve()
    {
        module = FrameworkModule();
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
        // Both optional (see the fields above): without them the bridge still
        // works, but the host's ImGui keeps no mouse position while we hold the
        // Win32 focus and its text fields deactivate on the next click.
        const bool haveMousePosQuery = ResolveOptional(isMousePosValid, "igIsMousePosValid");
        const bool haveMousePosEvent = ResolveOptional(addMousePosEvent, "ImGuiIO_AddMousePosEvent");
        if (!haveMousePosQuery || !haveMousePosEvent)
        {
            logger::warn("SKSE Menu Framework mouse-position exports unavailable; its text fields may deactivate on the first click after focus moves");
        }
        // Optional telemetry only (a missing one costs a log field, nothing else).
        (void)ResolveOptional(getActiveId, "igGetActiveID");
        (void)ResolveOptional(getHoveredId, "igGetHoveredID");
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
std::mutex                     s_pendingMutex;
std::deque<char16_t>           s_pending;

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
/// Framework frames (kAfterRender) seen since startup, and the count when the
/// current session began. The delta tells a session that "died because the host
/// stopped submitting the field" (a handful of frames, last one seconds ago) from
/// one whose ActiveId was cleared while the host kept rendering normally.
std::atomic<std::uint64_t> s_frameCount{0};
std::atomic<std::uint64_t> s_sessionBeginFrame{0};
/// PROBE(2026-10-12 pause/freeze session death): mouse presses the engine
/// delivered while the session owned the keyboard. A field that dies in the
/// same frame as a press is the framework's "release focus when we click
/// outside" rule (its ImGui has no mouse position while our ImeWnd holds the
/// Win32 focus, so any click reads as a click on nothing) — a different fix
/// from a state clear, and the two look identical in a session-end log.
std::atomic<std::uint32_t> s_sessionMousePresses{0};
/// PROBE(2026-10-12): Return/Escape presses seen during the session. ImGui's
/// InputText deactivates on either (validate / cancel), and both reach the host
/// through the engine's DirectInput stream even while our ImeWnd holds the Win32
/// focus — a session that ends without a mouse press but with one of these is
/// that path, not a click.
std::atomic<std::uint32_t> s_sessionReturnPresses{0};
std::atomic<std::uint32_t> s_sessionEscapePresses{0};
/// The host item that owned focus when the session began — the text field's
/// ImGui id, for "is it still the active item at the end?".
std::atomic<unsigned int> s_sessionFieldId{0};

/// The engine's pause counter, for the session-end probe: whether the game was
/// paused at the moment the host's field deactivated.
auto GamePauseCounter() -> std::uint32_t
{
    auto *ui = RE::UI::GetSingleton();
    return ui != nullptr ? ui->numPausesGame : 0;
}

/// True while the game's own window holds the Win32 foreground. The framework's
/// ImGui only tracks the OS pointer while this holds (its Win32 backend refreshes
/// MousePos under exactly this condition).
auto GameWindowFocused() -> bool
{
    const HWND gameWindow = Ime::ImeApp::GetInstance().GetGameHWND();
    return gameWindow != nullptr && ::GetForegroundWindow() == gameWindow;
}

/// Whether the framework's ImGui currently holds a usable mouse position.
/// True when the exports are unavailable (an older framework build): the repair
/// below is then skipped and this telemetry would only mislead.
auto HostMousePosValid() -> bool
{
    return s_api.isMousePosValid == nullptr || s_api.isMousePosValid(nullptr);
}

/// The host ImGui item that currently holds focus (0 = none), and the hovered id.
/// Telemetry: on a session end, "the field is still the active item" means the
/// field was never cleared — its window stopped being submitted instead.
auto HostActiveId() -> unsigned int
{
    return s_api.getActiveId != nullptr ? s_api.getActiveId() : 0U;
}

auto HostHoveredId() -> unsigned int
{
    return s_api.getHoveredId != nullptr ? s_api.getHoveredId() : 0U;
}

/// Set by the input-dispatch hook (game thread) when a mouse press arrives while a
/// session owns the keyboard. The framework's next frame must know where that
/// click was — see UpdateHostMousePosition.
std::atomic<bool> s_mousePressPending{false};
/// True between the injection and the end of the frame that consumed it (probe:
/// did the injected position actually reach the frame that judged the click?).
std::atomic<bool> s_injectedForPress{false};

/// PROBE(2026-10-12 SKSEMF session death): trace the host field's focus across
/// frames. A session ends either because the field's ActiveId was cleared (a
/// click/Enter/Esc — the click path needs a mouse position the host does not
/// have) or because the field stopped being submitted (its window closed); this
/// prints which, with the input state that frame ran with. `stage == nullptr`
/// seeds the comparison at a session begin.
void TraceHostFieldFocus(const char *stage)
{
    if (!s_sessionActive.load(std::memory_order_acquire))
    {
        return;
    }
    static unsigned int s_lastActiveId = 0;
    static bool         s_lastValid    = true;
    const auto          activeId       = HostActiveId();
    const bool          valid          = HostMousePosValid();
    if (stage == nullptr)
    {
        s_lastActiveId = activeId;
        s_lastValid    = valid;
        return;
    }
    if (activeId == s_lastActiveId && valid == s_lastValid)
    {
        return;
    }
    s_lastActiveId = activeId;
    s_lastValid    = valid;
    logger::debug(
        "Host field focus {}: activeId={:#x} (field {:#x}) mousePosValid={} hoveredId={:#x} gameWindowFocused={} numPausesGame={}",
        stage,
        activeId,
        s_sessionFieldId.load(std::memory_order_relaxed),
        valid,
        HostHoveredId(),
        GameWindowFocused(),
        GamePauseCounter()
    );
}

/// Offset of ImGuiIO::MousePos inside the framework's ImGui build. Derived from
/// the released DLL itself: disassembling its exported ImGuiIO_ClearInputKeys
/// shows the one ImVec2 it sets to (-FLT_MAX, -FLT_MAX) at io+0xF8, with MouseDown
/// at io+0x100 and MouseDownDuration/Prev at io+0x2B50/0x2B64 — the pre-KeysData
/// layout the two proven anchors (ConfigDebugIgnoreFocusLoss 0x7B, WantTextInput
/// 0xCC) pin down. A wrong offset would corrupt an unrelated io field, so the
/// direct write stays disabled until the slot is seen holding the OS cursor.
constexpr std::size_t IMGUI_IO_MOUSE_POS_OFFSET = 0xF8;
/// True once io+IMGUI_IO_MOUSE_POS_OFFSET has been observed to hold the OS cursor
/// (i.e. the offset is this build's MousePos and a direct write is safe).
std::atomic<bool> s_mousePosOffsetVerified{false};
/// Set when a frame wrote the position directly, so the end of that frame can put
/// the host back to "no mouse position" while the game window is unfocused (its
/// normal state — otherwise the host would draw its software cursor at a stale
/// spot).
std::atomic<bool> s_mousePosWroteDirect{false};

/// The position slot of the host's ImGuiIO, or nullptr when the offset is not
/// verified yet (or the io is gone).
auto HostMousePosSlot() -> float *
{
    if (!s_mousePosOffsetVerified.load(std::memory_order_acquire) || s_api.getIo == nullptr)
    {
        return nullptr;
    }
    auto *io = s_api.getIo();
    return io != nullptr ? reinterpret_cast<float *>(reinterpret_cast<std::uint8_t *>(io) + IMGUI_IO_MOUSE_POS_OFFSET) : nullptr;
}

/// Verify the MousePos offset from the live state: while the game window is
/// focused its own backend keeps that slot equal to the client-mapped cursor, so a
/// match proves the offset. Runs outside text sessions too (that is when the
/// signal exists).
void VerifyHostMousePosOffset()
{
    if (s_mousePosOffsetVerified.load(std::memory_order_acquire) || s_api.getIo == nullptr || !GameWindowFocused())
    {
        return;
    }
    auto *io = s_api.getIo();
    const HWND gameWindow = Ime::ImeApp::GetInstance().GetGameHWND();
    POINT    cursor{};
    if (io == nullptr || gameWindow == nullptr || ::GetCursorPos(&cursor) == FALSE || ::ScreenToClient(gameWindow, &cursor) == FALSE)
    {
        return;
    }
    const auto *slot = reinterpret_cast<const float *>(reinterpret_cast<const std::uint8_t *>(io) + IMGUI_IO_MOUSE_POS_OFFSET);
    // Two consecutive matches: one could be a coincidence (an uninitialised zero
    // slot and a cursor at the client origin), two cannot.
    static int s_matches = 0;
    if (std::abs(slot[0] - static_cast<float>(cursor.x)) <= 2.0F && std::abs(slot[1] - static_cast<float>(cursor.y)) <= 2.0F)
    {
        if (++s_matches >= 2)
        {
            s_mousePosOffsetVerified.store(true, std::memory_order_release);
            logger::info("SKSE Menu Framework io+{} verified as ImGuiIO::MousePos; direct mouse writes enabled", IMGUI_IO_MOUSE_POS_OFFSET);
        }
    }
    else
    {
        s_matches = 0;
    }
}

/// Feed the framework's ImGui the OS cursor for the frame that processes a click,
/// mapped into the game window's client area exactly like its own Win32 backend
/// does while the game window is focused.
///
/// Why this matters: ImGui's pointer position decides whether a click counts as
/// "inside the focused field". The backend refreshes it only while the GAME WINDOW
/// is focused, and our IME enable must move the Win32 focus to ImeWnd (TSF lives on
/// the IME thread) — on top of that, the framework's own WndProc hook clears its
/// whole input state, MousePos included, on WM_KILLFOCUS. Its ImGui is then left
/// with an invalid position, and InputText's "release focus when we click outside"
/// rule fires for EVERY later click: the user's own second click on the field (or
/// the press the engine re-emits when the input mode switches as the bar appears)
/// tears down the session it just opened — every 2026-10-12 "bar flashes off"
/// report carried a mouse press and a dead field, and none of them a paused game.
///
/// The position is WRITTEN INTO the io, not queued: ImGui's "trickle fast inputs"
/// rule (imgui.cpp, UpdateInputEvents) stops processing the event queue at a
/// MousePos event once a mouse-button change has been handled in that frame — and
/// the button event is always queued before we can react to the press, so a queued
/// position loses that race by construction and the click is judged with the
/// previous (cleared) position. The log of 2026-10-12 shows exactly that shape:
/// the click frame ends with mousePosValid=false and activeId=0, and the next frame
/// has valid=true with the field hovered again. Only the frames that carry a press
/// get a position: a permanently valid one would make the host draw its software
/// cursor over the game's own.
void UpdateHostMousePosition()
{
    if (!s_sessionActive.load(std::memory_order_acquire) || s_api.getIo == nullptr)
    {
        return;
    }
    if (!s_mousePressPending.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    const HWND gameWindow = Ime::ImeApp::GetInstance().GetGameHWND();
    if (gameWindow == nullptr)
    {
        return;
    }
    POINT cursor{};
    if (::GetCursorPos(&cursor) == FALSE || ::ScreenToClient(gameWindow, &cursor) == FALSE)
    {
        return;
    }
    if (float *slot = HostMousePosSlot(); slot != nullptr)
    {
        slot[0] = static_cast<float>(cursor.x);
        slot[1] = static_cast<float>(cursor.y);
        s_mousePosWroteDirect.store(true, std::memory_order_release);
        s_injectedForPress.store(true, std::memory_order_release);
        return;
    }
    // Offset not verified yet: fall back to the event path. It loses to the
    // trickle rule described above, but it is still better than nothing until the
    // next focused frame proves the offset.
    if (s_api.addMousePosEvent != nullptr)
    {
        if (auto *io = s_api.getIo(); io != nullptr)
        {
            s_api.addMousePosEvent(io, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
            s_injectedForPress.store(true, std::memory_order_release);
        }
    }
}

/// End of the frame that wrote the position directly: put the host back to "no
/// mouse position" while the game window is unfocused — that is the state its own
/// backend keeps (it only refreshes the position while focused), and leaving a
/// stale valid value behind would make the host draw its software cursor at an
/// old spot. When the game window IS focused the backend owns the value and keeps
/// it current, so it is left alone.
void RestoreHostMousePosAfterFrame()
{
    if (!s_mousePosWroteDirect.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    if (GameWindowFocused())
    {
        return;
    }
    // A live session keeps the position we just injected. While the game window
    // is unfocused its backend stops refreshing MousePos, so restoring "no
    // position" here leaves the host judging every later press against an
    // invalid cursor: InputText's "a click outside released the field" rule then
    // fires for presses it should ignore, and the field dies one or two frames
    // after the focus grab (in-game 2026-10-21: 47 ms session, beginActiveId
    // live, endActiveId=0, mousePosValid=false, zero mouse presses — nothing but
    // the position was wrong). The value always describes where the user really
    // clicked (only press frames write it) and ImGui draws its own cursor only
    // with io.MouseDrawCursor, so keeping it costs no visible artifact.
    if (s_sessionActive.load(std::memory_order_acquire))
    {
        return;
    }
    if (float *slot = HostMousePosSlot(); slot != nullptr)
    {
        slot[0] = -FLT_MAX;
        slot[1] = -FLT_MAX;
    }
}

/// Per framework frame, at kAfterRender: refresh the field anchor from
/// ImGui's own OS-IME caret report (PlatformImeData — what positions the
/// system IME window). Valid regardless of how the field gained focus,
/// including the auto-focused first field of a freshly opened menu.
///
/// The probe offset assumes vanilla 1.90.8; the framework's build may be a
/// patched fork whose ImGuiContext layout drifted. Calibrate at runtime: scan
/// the context for the WantVisible pattern while a field is active, prune
/// candidates that don't reset to 0 once the field deactivates. One survivor
/// wins; until then the anchor stays invalid (cursor fallback).

// Calibration state — touched only from OnFrameworkEvent (framework render
// callback thread); the resulting offset/flag are atomics for the read path.
constexpr std::size_t IMGUI_CTX_SCAN_MIN = 4096;
// Vanilla 1.90.8 offsetof(ImGuiContext, PlatformImeData) — the framework's
// fork shifted it (runtime calibration measured 24864/24880), so this is only
// the seed guess for the scan window, never trusted directly.
constexpr std::size_t IMGUI_CTX_PLATFORM_IME_DATA_OFFSET = 24384;
constexpr std::size_t IMGUI_CTX_SCAN_MAX = 25000;
constexpr std::size_t IMGUI_CAL_MAX_CANDIDATES = 64;
constexpr int IMGUI_CAL_MAX_EMPTY_ROUNDS = 3;

std::atomic<std::size_t> s_imeDataOffset{IMGUI_CTX_PLATFORM_IME_DATA_OFFSET};
std::atomic<bool>        s_imeDataOffsetReady{false};
bool        s_calActive = false;  ///< candidates pending, seen session-active
std::size_t s_calCandidates[IMGUI_CAL_MAX_CANDIDATES] = {};
std::size_t s_calCandidateCount = 0;
int         s_calFailedRounds = 0;
int         s_calStaleActiveReads = 0; ///< calibrated offset read WantVisible=0 while a field is active

/// The calibrated offset is a property of the framework DLL build (its
/// embedded imgui layout), so it is cached per framework fingerprint: a
/// framework update changes the fingerprint and forces one recalibration.
constexpr auto AnchorCachePath() -> std::filesystem::path
{
    return utils::GetPluginInterfaceDir() / "skse_menu_framework_anchor.cache";
}

void RestoreCalibratedImeDataOffset()
{
    std::ifstream file(AnchorCachePath());
    if (!file)
    {
        return;
    }
    std::uint64_t size = 0, mtime = 0;
    std::uint64_t cachedSize = 0, cachedMtime = 0, cachedOffset = 0;
    bool haveSize = false, haveMtime = false, haveOffset = false;
    for (std::string line; std::getline(file, line);)
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const auto key = line.substr(0, eq);
        const auto value = std::strtoull(line.c_str() + eq + 1, nullptr, 10);
        if (key == "framework_size") { cachedSize = value; haveSize = true; }
        else if (key == "framework_mtime") { cachedMtime = value; haveMtime = true; }
        else if (key == "ime_data_offset") { cachedOffset = value; haveOffset = true; }
    }
    file.close();
    if (!haveSize || !haveMtime || !haveOffset)
    {
        return;
    }
    if (!TryFrameworkIdentity(size, mtime))
    {
        return;
    }
    if (size != cachedSize || mtime != cachedMtime || cachedOffset < IMGUI_CTX_SCAN_MIN ||
        cachedOffset + sizeof(ImGuiPlatformImeDataMirror) > IMGUI_CTX_SCAN_MAX)
    {
        return;
    }
    s_imeDataOffset.store(static_cast<std::size_t>(cachedOffset), std::memory_order_release);
    s_imeDataOffsetReady.store(true, std::memory_order_release);
    logger::info("Restored calibrated PlatformImeData offset {} from cache (framework fingerprint match)", cachedOffset);
}

void SaveAnchorCache(std::size_t offset)
{
    std::uint64_t size = 0, mtime = 0;
    if (!TryFrameworkIdentity(size, mtime))
    {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(utils::GetPluginInterfaceDir(), ec);
    std::ofstream file(AnchorCachePath(), std::ios::trunc);
    if (!file)
    {
        return;
    }
    file << "framework_size=" << size << "\n"
         << "framework_mtime=" << mtime << "\n"
         << "ime_data_offset=" << offset << "\n";
}

/// Built-in offsets for framework builds seen in the wild, so a fresh install
/// anchors from the very first keystroke (a runtime cache can only exist
/// after this machine calibrated once). Keyed by DLL size + framework
/// version; a wrong entry self-heals — the calibrated read path recalibrates
/// after ~3s of a field being active with WantVisible stuck at 0.
struct KnownFrameworkImeOffset
{
    std::uint32_t dllSize;
    std::uint16_t frameworkVersionX100;
    std::size_t   imeDataOffset;
};
constexpr KnownFrameworkImeOffset kKnownImeOffsets[] = {
    { 4583936, 380, 24864 }, // SKSE-Menu-Framework 3.80 (embedded cimgui 1.90.8, shifted layout)
};

std::uint32_t FrameworkDllSize()
{
    std::uint64_t size = 0, mtime = 0;
    if (!TryFrameworkIdentity(size, mtime))
    {
        return 0;
    }
    return static_cast<std::uint32_t>(size);
}

void FinishCalibration(std::size_t offset)
{
    s_imeDataOffset.store(offset, std::memory_order_release);
    s_imeDataOffsetReady.store(true, std::memory_order_release);
    s_calActive = false;
    s_calCandidateCount = 0;
    logger::info("PlatformImeData calibrated at ImGuiContext+{} (probe {})", offset, IMGUI_CTX_PLATFORM_IME_DATA_OFFSET);
    SaveAnchorCache(offset);
}

bool ImeDataPatternPlausible(const ImGuiPlatformImeDataMirror *ime)
{
    // While visible, InputPos must be finite screen coords and the line
    // height a sane font size — filters a half-right offset read.
    return ime->wantVisible == 1 && ime->inputLineHeight >= 4.0F && ime->inputLineHeight <= 200.0F &&
           ime->inputPosX >= -2000.0F && ime->inputPosX <= 8000.0F && ime->inputPosY >= -2000.0F &&
           ime->inputPosY <= 8000.0F;
}

/// Keep only the candidates the predicate still accepts, in place; returns
/// the kept count (calibration converges by pruning).
template <typename Pred>
auto PruneCandidates(Pred &&keep) -> std::size_t
{
    std::size_t kept = 0;
    for (std::size_t i = 0; i < s_calCandidateCount; ++i)
    {
        if (keep(s_calCandidates[i]))
        {
            s_calCandidates[kept++] = s_calCandidates[i];
        }
    }
    s_calCandidateCount = kept;
    return kept;
}

/// The one warning both no-candidate give-up paths log (deactivated prune and
/// the initial scan): the anchor path is dead for this process.
void MarkAnchorPathBroken()
{
    s_imeDataPathBroken.store(true, std::memory_order_release);
    logger::warn("PlatformImeData calibration found no candidate; field anchors fall back to click-time cursor");
}

/// Uncalibrated mode, field deactivated: WantVisible must have reset to 0 on
/// every candidate — keep only the ones that did; persistent flags die here.
void UpdateFieldAnchorFieldGone(const std::uint8_t *base)
{
    if (s_calActive && s_calCandidateCount > 0)
    {
        const auto kept = PruneCandidates([base](const std::size_t offset) {
            return *reinterpret_cast<const std::uint8_t *>(base + offset) == 0;
        });
        if (kept == 1)
        {
            FinishCalibration(s_calCandidates[0]);
        }
        else if (kept == 0)
        {
            s_calActive = false;
            if (++s_calFailedRounds >= IMGUI_CAL_MAX_EMPTY_ROUNDS)
            {
                MarkAnchorPathBroken();
            }
        }
    }
    s_fieldAnchorValid.store(false, std::memory_order_release);
}

/// Uncalibrated mode, field active: initial scan, or prune candidates that
/// stopped matching. One survivor wins; zero ends the scan.
void UpdateFieldAnchorScanning(const std::uint8_t *base)
{
    if (!s_calActive)
    {
        s_calCandidateCount = 0;
        for (std::size_t o = IMGUI_CTX_SCAN_MIN;
             o + sizeof(ImGuiPlatformImeDataMirror) <= IMGUI_CTX_SCAN_MAX && s_calCandidateCount < IMGUI_CAL_MAX_CANDIDATES;
             o += 4)
        {
            const auto *ime = reinterpret_cast<const ImGuiPlatformImeDataMirror *>(base + o);
            if (ImeDataPatternPlausible(ime))
            {
                s_calCandidates[s_calCandidateCount++] = o;
            }
        }
        s_calActive = s_calCandidateCount > 0;
        if (!s_calActive && ++s_calFailedRounds >= IMGUI_CAL_MAX_EMPTY_ROUNDS)
        {
            MarkAnchorPathBroken();
        }
    }
    else
    {
        const auto kept = PruneCandidates([base](const std::size_t offset) {
            return ImeDataPatternPlausible(reinterpret_cast<const ImGuiPlatformImeDataMirror *>(base + offset));
        });
        if (kept == 1)
        {
            FinishCalibration(s_calCandidates[0]);
        }
        else if (kept == 0)
        {
            s_calActive = false;
        }
    }
    s_fieldAnchorValid.store(false, std::memory_order_release);
}

/// Calibrated read path. A stale cache (framework rebuilt without its
/// fingerprint changing is impossible, but a fingerprint collision or an
/// imgui change within the same file identity would read zeros) self-heals:
/// a field is active yet WantVisible stays 0 for ~3s → recalibrate.
void UpdateFieldAnchorCalibrated(const std::uint8_t *ctx, const bool wantsText)
{
    const auto *ime = reinterpret_cast<const ImGuiPlatformImeDataMirror *>(
        ctx + s_imeDataOffset.load(std::memory_order_relaxed));
    if (ime->wantVisible > 1)
    {
        s_imeDataPathBroken.store(true, std::memory_order_release);
        logger::warn("PlatformImeData read went implausible after calibration; field anchors fall back to click-time cursor");
        return;
    }
    if (ime->wantVisible != 0 && ImeDataPatternPlausible(ime))
    {
        s_calStaleActiveReads = 0;
        static std::atomic<bool> s_imeDataProbed{false};
        if (!s_imeDataProbed.exchange(true))
        {
            logger::info(
                "PlatformImeData caret anchor live at ImGuiContext+{}: pos=({:.1f},{:.1f}) lineHeight={:.1f}",
                s_imeDataOffset.load(std::memory_order_relaxed),
                ime->inputPosX,
                ime->inputPosY,
                ime->inputLineHeight);
        }
        // InputPos is the caret line's top-left in screen space; anchor at its
        // bottom so the candidate window hangs just below the text line.
        s_fieldAnchorX.store(ime->inputPosX, std::memory_order_release);
        s_fieldAnchorY.store(ime->inputPosY + ime->inputLineHeight, std::memory_order_release);
        s_fieldAnchorValid.store(true, std::memory_order_release);
        return;
    }
    if (wantsText && ime->wantVisible == 0)
    {
        if (++s_calStaleActiveReads > 180)
        {
            s_calStaleActiveReads = 0;
            s_imeDataOffsetReady.store(false, std::memory_order_release);
            logger::warn("Cached PlatformImeData offset stopped matching; recalibrating on the next session");
        }
        return;
    }
    s_calStaleActiveReads = 0;
    // No active InputText (or implausible data): the anchor must not outlive
    // its field.
    s_fieldAnchorValid.store(false, std::memory_order_release);
}

void UpdateFieldAnchor()
{
    if (s_platformImeHookInstalled.load(std::memory_order_acquire))
    {
        // The SetPlatformImeDataFn hook owns the anchor lifecycle (activation,
        // caret move, deactivation all arrive as events). No polling needed.
        return;
    }
    if (s_api.getCurrentContext == nullptr || s_api.getIo == nullptr)
    {
        return;
    }
    auto *ctx = s_api.getCurrentContext();
    auto *io = s_api.getIo();
    if (ctx == nullptr || io == nullptr || s_imeDataPathBroken.load(std::memory_order_relaxed))
    {
        return;
    }
    const bool wantsText = *reinterpret_cast<const volatile bool *>(
        reinterpret_cast<const std::uint8_t *>(io) + IMGUI_IO_WANT_TEXT_INPUT_OFFSET);

    if (!s_imeDataOffsetReady.load(std::memory_order_acquire))
    {
        const auto *base = reinterpret_cast<const std::uint8_t *>(ctx);
        if (!wantsText)
        {
            UpdateFieldAnchorFieldGone(base);
            return;
        }
        UpdateFieldAnchorScanning(base);
        return;
    }
    UpdateFieldAnchorCalibrated(reinterpret_cast<const std::uint8_t *>(ctx), wantsText);
}

void BeginTextInput()
{
    AcquireTextEntryLease();
    s_sessionBeginMs.store(GetTickCount64(), std::memory_order_release);
    s_imeDisabledAtBegin.store(Ime::Core::State::GetInstance().ImeDisabled(), std::memory_order_release);
    s_sessionMousePresses.store(0, std::memory_order_relaxed);
    s_sessionReturnPresses.store(0, std::memory_order_relaxed);
    s_sessionEscapePresses.store(0, std::memory_order_relaxed);
    s_sessionBeginFrame.store(s_frameCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s_sessionFieldId.store(HostActiveId(), std::memory_order_relaxed);
    s_mousePressPending.store(false, std::memory_order_relaxed);
    s_injectedForPress.store(false, std::memory_order_relaxed);
    // Fallback anchor when no live source exists (SetPlatformImeDataFn hook
    // failed to install and no calibrated offset): the engine cursor is on
    // the field the user just clicked. Trust MenuCursor under the same
    // condition imgui_manager does — the singleton always exists, and with
    // the CursorMenu closed its coords are stale.
    if (s_imeDataPathBroken.load(std::memory_order_acquire) ||
        (!s_platformImeHookInstalled.load(std::memory_order_acquire) &&
         !s_imeDataOffsetReady.load(std::memory_order_acquire)))
    {
        auto *ui = RE::UI::GetSingleton();
        if (ui != nullptr && ui->IsMenuOpen(RE::CursorMenu::MENU_NAME))
        {
            if (const auto *cursor = RE::MenuCursor::GetSingleton(); cursor != nullptr)
            {
                s_fieldAnchorX.store(cursor->cursorPosX, std::memory_order_release);
                s_fieldAnchorY.store(cursor->cursorPosY, std::memory_order_release);
                s_fieldAnchorValid.store(true, std::memory_order_release);
            }
        }
    }
    logger::info("SKSE Menu Framework text session begin (lease acquired)");
    TraceHostFieldFocus(nullptr);
}

void EndTextInput()
{
    ReleaseTextEntryLease();
    // Same contract as ForceEndSession: text queued for the field that just
    // ended is stale and must not survive into the next session's field
    // (FlushPending only checks s_sessionActive, which the next session
    // re-arms).
    {
        const std::scoped_lock lock(s_pendingMutex);
        if (!s_pending.empty())
        {
            logger::debug("Discarded {} queued committed unit(s) on session end", s_pending.size());
            s_pending.clear();
        }
    }
    const auto fieldId = s_sessionFieldId.load(std::memory_order_relaxed);
    logger::info(
        "SKSE Menu Framework text session end (lease released, held {} ms, {} mouse press(es), {} return/{} escape during the session, "
        "numPausesGame={}, hostMousePosValid={}, gameWindowFocused={}, fieldActive={} activeId={:#x} hoveredId={:#x})",
        GetTickCount64() - s_sessionBeginMs.load(std::memory_order_relaxed),
        s_sessionMousePresses.load(std::memory_order_relaxed),
        s_sessionReturnPresses.load(std::memory_order_relaxed),
        s_sessionEscapePresses.load(std::memory_order_relaxed),
        GamePauseCounter(),
        HostMousePosValid(),
        GameWindowFocused(),
        fieldId != 0U && HostActiveId() == fieldId,
        HostActiveId(),
        HostHoveredId()
    );
    // A field that flashes and dies needs a different fix depending on WHY:
    //   * the host kept submitting frames but the field's ActiveId was cleared
    //     (a click/Enter reached ImGui) -> scrub the replayed input;
    //   * the host stopped submitting the field (no frames, or frames without it)
    //     -> nothing in the input layer can save it.
    // Frame delta + last-frame age separate the two.
    const auto heldMs = GetTickCount64() - s_sessionBeginMs.load(std::memory_order_relaxed);
    if (heldMs < 200)
    {
        const auto now           = GetTickCount64();
        const auto lastEventMs   = s_lastFrameworkEventMs.load(std::memory_order_relaxed);
        const auto framesInSession = s_frameCount.load(std::memory_order_relaxed) - s_sessionBeginFrame.load(std::memory_order_relaxed);
        logger::info(
            "Short text session probe: {} framework frame(s) in {} ms, last framework event {} ms ago, beginActiveId={:#x} endActiveId={:#x} mousePosValid={} hoveredId={:#x}",
            framesInSession,
            heldMs,
            lastEventMs == 0 ? 0 : now - lastEventMs,
            fieldId,
            HostActiveId(),
            HostMousePosValid(),
            HostHoveredId()
        );
    }
}

// ---- host frame ring (diagnostics) -------------------------------------
// One sample per frame-poll. A session that dies in one or two frames leaves no
// trace of WHY in the session-end line — that line only carries the final state,
// and "begin id live, end id dead" is consistent with a click closing the field,
// a window that stopped being submitted, and a host that never opened a field at
// all. Keeping the last few frames separates them.
struct HostFrameSample
{
    std::uint64_t ms;
    unsigned int  activeId;
    unsigned int  hoveredId;
    bool          posValid;
    bool          want;
};
constexpr int   kHostFrameRing      = 16;
HostFrameSample s_hostFrameRing[kHostFrameRing]{};
int             s_hostFrameRingPos   = 0;
int             s_hostFrameRingCount = 0;

void PushHostFrameSample(const bool want)
{
    auto &slot      = s_hostFrameRing[s_hostFrameRingPos];
    slot.ms         = GetTickCount64();
    slot.activeId   = HostActiveId();
    slot.hoveredId  = HostHoveredId();
    slot.posValid   = HostMousePosValid();
    slot.want       = want;
    s_hostFrameRingPos = (s_hostFrameRingPos + 1) % kHostFrameRing;
    if (s_hostFrameRingCount < kHostFrameRing)
    {
        ++s_hostFrameRingCount;
    }
}

/// Dump the ring oldest-first, compactly: "+<ms> a=<activeId> h=<hoveredId>
/// p=<posValid> w=<want>". Every ms value is an offset from the oldest sample.
void DumpHostFrameRing(const char *why)
{
    if (s_hostFrameRingCount == 0)
    {
        return;
    }
    const int   first = (s_hostFrameRingPos - s_hostFrameRingCount + kHostFrameRing) % kHostFrameRing;
    const auto  base  = s_hostFrameRing[first].ms;
    std::string line;
    line.reserve(64 + 40 * static_cast<std::size_t>(s_hostFrameRingCount));
    for (int i = 0; i < s_hostFrameRingCount; ++i)
    {
        const auto &s = s_hostFrameRing[(first + i) % kHostFrameRing];
        char buf[128];
        std::snprintf(
            buf,
            sizeof(buf),
            " | +%llums a=%#x h=%#x p=%d w=%d",
            static_cast<unsigned long long>(s.ms - base),
            s.activeId,
            s.hoveredId,
            s.posValid ? 1 : 0,
            s.want ? 1 : 0
        );
        line += buf;
    }
    logger::info("Host frame ring ({}):{}", why, line);
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
    const bool rawWants = *reinterpret_cast<const volatile bool *>(reinterpret_cast<const std::uint8_t *>(io) + IMGUI_IO_WANT_TEXT_INPUT_OFFSET);
    // Only a want with a live item behind it is a field the IME can type into:
    // after a commit the host keeps WantTextInput set for a frame or two with no
    // active item, and starting a session on that residue grabs the focus for
    // nothing (2026-10-21: three 31 ms sessions, beginActiveId=0x0, right after
    // an Enter). Filtering here rather than inside BeginTextInput keeps the
    // edge detection below honest.
    // ...and it has to persist, and it has to be the SAME item: the host reports
    // a want for single-frame events that open no lasting field, which is how a
    // 31 ms session with a live begin id and a dead end id happens (see
    // IsStableWant). Both counters are touched from this callback thread only.
    static unsigned int  s_lastWantActiveId = 0;
    static int           s_wantStreak       = 0;
    static std::uint64_t s_lastWantMs       = 0;
    const unsigned int  activeId           = HostActiveId();
    const bool          wantThisFrame =
        SkseMenuFrameworkBridgeLogic::ShouldStartSession(rawWants, s_api.getActiveId != nullptr, activeId);
    const int streakBefore = s_wantStreak;
    if (!wantThisFrame)
    {
        s_lastWantActiveId = 0;
        s_wantStreak       = 0;
    }
    else if (activeId == s_lastWantActiveId)
    {
        if (s_wantStreak < 8)
        {
            ++s_wantStreak; // diagnostics only: how many frames this want has run
        }
    }
    else
    {
        s_lastWantActiveId = activeId;
        s_wantStreak       = 1;
    }
    // A want of one frame still opens a session; the hangover is what keeps it
    // alive long enough for the lease and the IME to do their work (see
    // ShouldHoldSession).
    const auto nowMs = GetTickCount64();
    if (wantThisFrame)
    {
        s_lastWantMs = nowMs;
    }
    const bool holds = SkseMenuFrameworkBridgeLogic::ShouldHoldSession(
        wantThisFrame,
        s_lastWantMs == 0 ? -1 : static_cast<int>(nowMs - s_lastWantMs));
    // Sample every frame, and dump when a short-lived want ends: that is the
    // shape a session we refused to start would have left no other trace of.
    PushHostFrameSample(wantThisFrame);
    if (!wantThisFrame && streakBefore > 0 && streakBefore <= 3)
    {
        DumpHostFrameRing("a want that did not last");
    }
    const bool previous = s_sessionActive.exchange(holds, std::memory_order_acq_rel);
    if (holds == previous)
    {
        return;
    }
    if (holds)
    {
        BeginTextInput();
    }
    else
    {
        EndTextInput();
    }
}

// ---- committed text injection (framework callback thread) --------------
/// The neutralizer's decision for the current frame, read from the live state.
/// One source of truth for both sides of the delivery contract: the raw-ASCII
/// filter below (which drops the engine's echoes of IME-owned keys) and the
/// injection gate (which has to carry exactly the characters that filter drops).
auto RawAsciiEchoSuppressed() -> bool
{
    const auto &state = Ime::Core::State::GetInstance();
    const bool  freshEnableTransition =
        s_imeDisabledAtBegin.load(std::memory_order_acquire) &&
        (GetTickCount64() - s_sessionBeginMs.load(std::memory_order_acquire)) < TRANSITION_GRACE_MS;
    const bool tipActive = state.Has(Ime::Core::State::INPUT_PROCESSOR_ACTIVATED);
    return SkseMenuFrameworkBridgeLogic::ShouldNeutralizeAscii(
        /*sessionActive=*/true,
        state.IsImeInputting(),
        state.ImeDisabled(),
        state.IsKeyboardOpen(),
        state.GetConversionMode().IsNative(),
        freshEnableTransition,
        tipActive
    );
}

void InjectCodepoint(unsigned int codepoint)
{
    // Printable ASCII only while the engine's echo of that key is suppressed;
    // non-ASCII needs a valid scalar (see the logic header). The TIP hands its
    // passed-through characters (English mode, space, - +) to ImeWnd, so this
    // gate is what makes them reach the field exactly once.
    if (!SkseMenuFrameworkBridgeLogic::ShouldInjectCodepoint(codepoint, RawAsciiEchoSuppressed()))
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
    if (eventType == EVENT_AFTER_RENDER)
    {
        s_frameCount.fetch_add(1, std::memory_order_relaxed);
        // ImGui's InputText wrote this frame's OS-IME caret into
        // PlatformImeData during widget submission — read it here, after the
        // frame, where it is guaranteed current for the next session check.
        UpdateFieldAnchor();
        RestoreHostMousePosAfterFrame();
        TraceHostFieldFocus("after the frame");
        // PROBE(2026-10-12): the frame that judged a click has just finished —
        // did the injected position actually reach it, and is the field still the
        // active item?
        if (s_injectedForPress.exchange(false, std::memory_order_acq_rel))
        {
            logger::debug(
                "Host mouse position after the click frame: valid={} activeId={:#x} (field {:#x})",
                HostMousePosValid(),
                HostActiveId(),
                s_sessionFieldId.load(std::memory_order_relaxed)
            );
        }
        return;
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
            auto *ioBytes = reinterpret_cast<std::uint8_t *>(io);
            // A fresh io has this flag off. A non-zero byte means the offset
            // does not point where this build thinks (host ABI drift): write
            // nothing — a blind write could corrupt a neighboring flag, and
            // the read-back below could not tell ours from a pre-existing 1.
            if (ioBytes[IMGUI_IO_CONFIG_IGNORE_FOCUS_LOSS_OFFSET] != 0)
            {
                logger::warn(
                    "SKSE Menu Framework io byte at offset {:#x} is not 0 before our write (ABI drift?); not enabling ConfigDebugIgnoreFocusLoss",
                    IMGUI_IO_CONFIG_IGNORE_FOCUS_LOSS_OFFSET);
            }
            else
            {
                ioBytes[IMGUI_IO_CONFIG_IGNORE_FOCUS_LOSS_OFFSET] = 1;
                if (ioBytes[IMGUI_IO_CONFIG_IGNORE_FOCUS_LOSS_OFFSET] != 1)
                {
                    logger::error("SKSE Menu Framework io write did not stick (offset {:#x})",
                                  IMGUI_IO_CONFIG_IGNORE_FOCUS_LOSS_OFFSET);
                }
                else
                {
                    logger::info("SKSE Menu Framework io: ConfigDebugIgnoreFocusLoss enabled (our IME focus moves must not clear its input state)");
                }
            }
        }
    }

    // Flush BEFORE the transition check: the final commit of a session races
    // the field's deactivation, and injecting one frame late is harmless
    // (ImGui discards unconsumed input) while dropping it eats the last char.
    FlushPending();
    UpdateFieldAnchor();
    // The offset proof lives in frames where the game window is focused — which
    // is every frame outside a session, so this runs unconditionally.
    VerifyHostMousePosOffset();
    // Before the transition check too: this frame's widget submission is what
    // decides hover/click ownership, so the position must be in the queue that
    // NewFrame is about to consume.
    UpdateHostMousePosition();
    TraceHostFieldFocus("before the frame");
    UpdateTextInputState();
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
    const ScopeFlag resolveInFlight(s_resolveInFlight);
    if (!resolveInFlight.owned())
    {
        return false;
    }

    const auto now      = GetTickCount64();
    bool       eligible = s_lastFrameworkEventMs.load(std::memory_order_relaxed) != 0 ||
                    (now - s_lastResolveAttemptMs.load(std::memory_order_relaxed)) >= RESOLVE_RETRY_MS;
    if (!eligible)
    {
        return false;
    }
    s_lastResolveAttemptMs.store(now, std::memory_order_relaxed);

    if (!s_api.Resolve())
    {
        s_state = SupportState::NotDetected;
        logger::info("SKSEMenuFramework.dll not loaded (yet); SKSE Menu Framework input support stays off");
        return false;
    }

    const float version = s_api.getVersion();
    if (version + 0.0001F < MIN_FRAMEWORK_VERSION)
    {
        s_state = SupportState::Failed;
        logger::warn("SKSE Menu Framework {:.2f} is too old; {:.2f}+ required for IME input support", version, MIN_FRAMEWORK_VERSION);
        return false;
    }

    s_api.registerEventPriority(&OnFrameworkEvent, EVENT_PRIORITY);
    // Framework DLL identity is known here — restore the calibrated anchor
    // offset, from this machine's cache first (exact fingerprint), then from
    // the built-in table for known framework builds (fresh installs). Order:
    // exact cache → built-in → runtime calibration on first use.
    RestoreCalibratedImeDataOffset();
    if (!s_imeDataOffsetReady.load(std::memory_order_acquire))
    {
        const auto versionX100 = static_cast<std::uint16_t>(version * 100.0F + 0.5F);
        const auto dllSize = FrameworkDllSize();
        for (const auto &known : kKnownImeOffsets)
        {
            if (known.dllSize == dllSize && known.frameworkVersionX100 == versionX100)
            {
                s_imeDataOffset.store(known.imeDataOffset, std::memory_order_release);
                s_imeDataOffsetReady.store(true, std::memory_order_release);
                logger::info(
                    "Using built-in PlatformImeData offset {} for framework {:.2f} (dll {} bytes)",
                    known.imeDataOffset,
                    version,
                    dllSize);
                break;
            }
        }
    }
    // Primary, layout-independent source: hook the fn pointer ImGui calls with
    // &g.PlatformImeData on every change. Installation self-validates (the
    // slot must hold a pointer into the framework module); on failure the
    // calibration/click fallbacks above take over.
    InstallPlatformImeDataHook(s_api.getIo());

    // The raw-ASCII filter itself is not installed here: the detour on the
    // dispatch call is shared with every self-drawn ImGui host (see
    // Hooks::InputDispatchHook, installed at startup) and calls
    // NeutralizeRawAscii from its thunk. Nothing else to do on this side.

    s_apiReady.store(true, std::memory_order_release);
    s_state = SupportState::Active;
    logger::info("SKSE Menu Framework bridge ready (framework {:.2f}, input filter installed)", version);
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
    if (!BridgeGate::ConfigOpen(s_enabled.load(), "SKSE Menu Framework input support"))
    {
        s_state = SupportState::Off;
        return;
    }

    // Mutual exclusion with the standalone TMS_SIMEtoSKSEMF bridge: it performs
    // the very same lease + injection dance, so both active at once would
    // double-inject every commit (same standoff semantics as
    // MeridianBridge::Install's rival-plugin check).
    if (!BridgeGate::ClearOfRival(L"TMS_SIMEtoSKSEMF.dll", "TMS_SIMEtoSKSEMF",
                                  "SimpleIME's built-in SKSE Menu Framework input support"))
    {
        s_state = SupportState::Standoff;
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

bool HasFieldAnchor()
{
    return s_fieldAnchorValid.load(std::memory_order_acquire);
}

void GetFieldAnchor(float &a_x, float &a_y)
{
    a_x = s_fieldAnchorX.load(std::memory_order_acquire);
    a_y = s_fieldAnchorY.load(std::memory_order_acquire);
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
        // Same strip list as the Scaleform commit path (see the predicate).
        if (Ime::ShouldStripCommittedChar(c))
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
        const auto fieldId = s_sessionFieldId.load(std::memory_order_relaxed);
        logger::info(
            "SKSE Menu Framework text session force-ended (held {} ms, {} mouse press(es), {} return/{} escape during the session, "
            "numPausesGame={}, hostMousePosValid={}, gameWindowFocused={}, fieldActive={} activeId={:#x} hoveredId={:#x})",
            GetTickCount64() - s_sessionBeginMs.load(std::memory_order_relaxed),
            s_sessionMousePresses.load(std::memory_order_relaxed),
            s_sessionReturnPresses.load(std::memory_order_relaxed),
            s_sessionEscapePresses.load(std::memory_order_relaxed),
            GamePauseCounter(),
            HostMousePosValid(),
            GameWindowFocused(),
            HostActiveId() == fieldId,
            HostActiveId(),
            HostHoveredId()
        );
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

    // PROBE(2026-10-12 pause/freeze session death): every input batch during a
    // session passes through here, so this is where a press that kills the host's
    // field can be counted (see s_sessionMousePresses) — and where the state that
    // makes a press fatal is visible: the framework's ImGui reads a click with no
    // mouse position as a click on nothing, which deactivates its InputText.
    for (auto *event = *events; event != nullptr; event = event->next)
    {
        const auto *button = event->AsButtonEvent();
        if (button == nullptr || !button->IsPressed())
        {
            continue;
        }
        if (button->GetDevice() == RE::INPUT_DEVICE::kMouse)
        {
            s_sessionMousePresses.fetch_add(1, std::memory_order_relaxed);
            // The host's next ImGui frame needs the click position to match it
            // against the focused field's rectangle (see UpdateHostMousePosition).
            s_mousePressPending.store(true, std::memory_order_release);
            logger::debug(
                "Mouse press during the session: hostMousePosValid={} gameWindowFocused={} activeId={:#x} (field {:#x})",
                HostMousePosValid(),
                GameWindowFocused(),
                HostActiveId(),
                s_sessionFieldId.load(std::memory_order_relaxed)
            );
        }
        else if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard)
        {
            // InputText's other two deactivation paths (validate / cancel).
            if (button->GetIDCode() == REX::W32::DIK_RETURN)
            {
                s_sessionReturnPresses.fetch_add(1, std::memory_order_relaxed);
            }
            else if (button->GetIDCode() == REX::W32::DIK_ESCAPE)
            {
                s_sessionEscapePresses.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (!RawAsciiEchoSuppressed())
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
