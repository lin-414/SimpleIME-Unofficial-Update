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
#include "RE/C/CursorMenu.h"
#include "RE/ControlMap.h"
#include "RE/M/MenuCursor.h"
#include "core/State.h"
#include "hook.h"
#include "hooks/SkseMenuFrameworkBridgeLogic.h"
#include "hooks/ScopeFlag.h"
#include "log.h"
#include "path_utils.h"

#include <REL/REL.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
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
    const HMODULE frameworkModule = GetModuleHandleW(L"SKSEMenuFramework.dll");
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
    std::uint64_t size = 0, mtime = 0, offset = 0;
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
    const HMODULE module = GetModuleHandleW(L"SKSEMenuFramework.dll");
    wchar_t path[MAX_PATH] = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0 ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
    {
        return;
    }
    size  = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    mtime = (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
            attributes.ftLastWriteTime.dwLowDateTime;
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
    const HMODULE module = GetModuleHandleW(L"SKSEMenuFramework.dll");
    wchar_t path[MAX_PATH] = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0 ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
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
    file << "framework_size=" << ((static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow) << "\n"
         << "framework_mtime=" << ((static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
                                   attributes.ftLastWriteTime.dwLowDateTime) << "\n"
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
    const HMODULE module = GetModuleHandleW(L"SKSEMenuFramework.dll");
    wchar_t path[MAX_PATH] = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0 ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
    {
        return 0;
    }
    return attributes.nFileSizeLow;
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
            if (s_calActive && s_calCandidateCount > 0)
            {
                // Field deactivated: WantVisible must have reset to 0. Keep
                // only candidates that did — persistent flags die here.
                std::size_t kept = 0;
                for (std::size_t i = 0; i < s_calCandidateCount; ++i)
                {
                    if (*reinterpret_cast<const std::uint8_t *>(base + s_calCandidates[i]) == 0)
                    {
                        s_calCandidates[kept++] = s_calCandidates[i];
                    }
                }
                s_calCandidateCount = kept;
                if (kept == 1)
                {
                    FinishCalibration(s_calCandidates[0]);
                }
                else if (kept == 0)
                {
                    s_calActive = false;
                    if (++s_calFailedRounds >= IMGUI_CAL_MAX_EMPTY_ROUNDS)
                    {
                        s_imeDataPathBroken.store(true, std::memory_order_release);
                        logger::warn("PlatformImeData calibration found no candidate; field anchors fall back to click-time cursor");
                    }
                }
            }
            s_fieldAnchorValid.store(false, std::memory_order_release);
            return;
        }

        // Field active: initial scan or prune candidates that stopped matching.
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
                s_imeDataPathBroken.store(true, std::memory_order_release);
                logger::warn("PlatformImeData calibration found no candidate; field anchors fall back to click-time cursor");
            }
        }
        else
        {
            std::size_t kept = 0;
            for (std::size_t i = 0; i < s_calCandidateCount; ++i)
            {
                const auto *ime = reinterpret_cast<const ImGuiPlatformImeDataMirror *>(base + s_calCandidates[i]);
                if (ImeDataPatternPlausible(ime))
                {
                    s_calCandidates[kept++] = s_calCandidates[i];
                }
            }
            s_calCandidateCount = kept;
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
        return;
    }

    // Calibrated read path. A stale cache (framework rebuilt without its
    // fingerprint changing is impossible, but a fingerprint collision or an
    // imgui change within the same file identity would read zeros) self-heals:
    // a field is active yet WantVisible stays 0 for ~3s → recalibrate.
    const auto *ime = reinterpret_cast<const ImGuiPlatformImeDataMirror *>(
        reinterpret_cast<const std::uint8_t *>(ctx) + s_imeDataOffset.load(std::memory_order_relaxed));
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

void BeginTextInput()
{
    AcquireTextEntryLease();
    s_sessionBeginMs.store(GetTickCount64(), std::memory_order_release);
    s_imeDisabledAtBegin.store(Ime::Core::State::GetInstance().ImeDisabled(), std::memory_order_release);
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
    if (eventType == EVENT_AFTER_RENDER)
    {
        // ImGui's InputText wrote this frame's OS-IME caret into
        // PlatformImeData during widget submission — read it here, after the
        // frame, where it is guaranteed current for the next session check.
        UpdateFieldAnchor();
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

    // Flush BEFORE the transition check: the final commit of a session races
    // the field's deactivation, and injecting one frame late is harmless
    // (ImGui discards unconsumed input) while dropping it eats the last char.
    FlushPending();
    UpdateFieldAnchor();
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

    // Install the raw-ASCII filter only when the framework is actually present
    // (the detour is inert outside a session, but there is no reason to take
    // it for users without the framework at all). Same single-threaded-install
    // invariant as the other HookData users: this runs on the main thread at
    // kDataLoaded (or the game thread in Tick) before a session can exist.
    if (!s_hookInstalled.exchange(true, std::memory_order_acq_rel))
    {
        s_inputHook = std::make_unique<DispatchInputEventHookData>(&DispatchInputEventThunk);
        if (s_inputHook->GetAddress() == 0)
        {
            logger::error("SKSE Menu Framework input filter resolved to address 0; the ASCII filter is inert");
        }
    }

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
