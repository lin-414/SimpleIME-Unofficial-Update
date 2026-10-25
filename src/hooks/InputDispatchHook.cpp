#include "hooks/InputDispatchHook.h"

#include "core/State.h"
#include "hook.h"
#include "hooks/InputDispatchLogic.h"
#include "hooks/SkseMenuFrameworkBridge.h"
#include "log.h"
#include "utils/Utils.h"

#include <RE/B/ButtonEvent.h>
#include <RE/I/InputDevices.h>
#include <RE/I/InputEvent.h>
#include <REX/W32/DINPUT.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace Hooks
{
namespace
{
/// Never uninstalled: outside a composition it is a pure passthrough, and the
/// trampoline cannot be safely torn down while the game may be inside it.
std::unique_ptr<DispatchInputEventHookData> s_inputHook;
std::atomic<bool>                          s_installAttempted{false};

/// How long after our focus grab the engine's replays of held buttons are
/// scrubbed. Long enough to cover the WM_KILLFOCUS -> ClearInputKeys() ->
/// next-poll round trip (one or two frames at 60 fps), short enough that the
/// user's own next key or click is never affected.
constexpr int kFocusGrabReplayWindowMs = 300;

/// Monotonic time of the last focus grab in milliseconds, or -1 when the focus
/// has never been grabbed by us. Written from the IME thread, read from the game
/// thread inside the dispatch thunk.
std::atomic<std::int64_t> s_focusGrabMs{-1};

/// Buttons physically held at the moment of the last focus grab: bit 0 Enter,
/// bits 1..5 mouse buttons 0..4 (left, right, middle, X1, X2). Only a button
/// that was held at the grab can come back as a replayed press, so only those
/// are scrubbed — a click or Enter that begins after the grab is the user's
/// real input and flows through untouched.
std::atomic<std::uint32_t> s_focusGrabHeldButtons{0};

constexpr std::uint32_t kEnterBit = 1u << 0;
constexpr std::uint32_t kMouseBit = 1u << 1; ///< mouse button i occupies bit (1 + i)

auto NowMs() -> std::int64_t
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/// True when the text the user is typing into belongs to a surface that draws its
/// own ImGui. Scaleform fields and the console have no ImGui input state to kill
/// and treat Enter as "execute", so they are excluded.
bool TargetsSelfDrawnImGuiHost()
{
    return SkseMenuFrameworkBridge::SessionActive() || Ime::Skyrim::IsExternalImGuiSurface() ||
           Ime::Skyrim::IsTextEntryCountOwnedByImGuiHost();
}

/// Zero a composition's Enter before it reaches any sink: the hosts read the
/// confirm key from this batch and deactivate the field on it (InputText's
/// validate path), which tears down the session that the commit needs.
void NeutralizeCompositionCommitKey(RE::InputEvent *const *a_events)
{
    if (a_events == nullptr || *a_events == nullptr)
    {
        return;
    }
    const auto &state = Ime::Core::State::GetInstance();
    if (!InputDispatchLogic::ShouldSwallowCommitKey(
            state.Has(Ime::Core::State::IN_COMPOSING),
            state.ImeDisabled(),
            TargetsSelfDrawnImGuiHost()))
    {
        return;
    }

    std::uint32_t swallowed = 0;
    for (auto *event = *a_events; event != nullptr; event = event->next)
    {
        auto *button = event->AsButtonEvent();
        if (button == nullptr || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
        {
            continue;
        }
        // Both Enter scan codes: the main block key and the numeric keypad one
        // validate an InputText the same way.
        const auto idCode = button->GetIDCode();
        if (idCode != REX::W32::DIK_RETURN && idCode != REX::W32::DIK_NUMPADENTER)
        {
            continue;
        }
        auto &runtime = button->GetRuntimeData();
        if (runtime.value <= 0.0F)
        {
            continue; // key release: the host needs it to clear its own key state
        }
        runtime.value         = 0.0F;
        runtime.heldDownSecs  = 0.0F;
        ++swallowed;
    }
    if (swallowed != 0)
    {
        // Info, not debug: this is the only in-game evidence that the filter is
        // ahead of the host in the dispatch chain. A session that still ends on
        // Enter with no line here means our hook sits *inside* another plugin's.
        logger::info("Swallowed {} composition Enter press(es) before the engine dispatched them", swallowed);
    }
}

/// Scrub the presses that are really echoes of our own focus grab.
///
/// Moving the Win32 focus to the IME window makes the hosting plugin's WndProc
/// clear ImGui's key and mouse state; the engine's next poll then hands the same
/// physical buttons back as new presses. For an InputText that is being opened
/// right now that is fatal in two ways:
///   * Enter -> validate (the field closes immediately);
///   * mouse press -> "clicked outside the field" (InputTextEx releases focus).
/// Only presses of buttons that were physically held at the grab are zeroed —
/// input that begins after the grab is the user's own; releases still flow so
/// the host can settle its state.
void SuppressFocusGrabReplays(RE::InputEvent *const *a_events)
{
    if (a_events == nullptr || *a_events == nullptr)
    {
        return;
    }
    const auto grabbedMs = s_focusGrabMs.load(std::memory_order_relaxed);
    if (grabbedMs < 0)
    {
        return;
    }
    const auto elapsedMs = static_cast<int>(NowMs() - grabbedMs);
    if (!InputDispatchLogic::IsFocusGrabReplay(elapsedMs, kFocusGrabReplayWindowMs))
    {
        return;
    }
    const auto heldButtons = s_focusGrabHeldButtons.load(std::memory_order_relaxed);

    std::uint32_t keys   = 0;
    std::uint32_t clicks = 0;
    for (auto *event = *a_events; event != nullptr; event = event->next)
    {
        auto       *button  = event->AsButtonEvent();
        if (button == nullptr)
        {
            continue;
        }
        const auto  device  = button->GetDevice();
        auto       &runtime = button->GetRuntimeData();
        if (runtime.value <= 0.0F)
        {
            continue; // release: let it through, the host clears its state with it
        }
        if (device == RE::INPUT_DEVICE::kKeyboard)
        {
            const auto idCode = button->GetIDCode();
            if (idCode != REX::W32::DIK_RETURN && idCode != REX::W32::DIK_NUMPADENTER)
            {
                continue;
            }
            if ((heldButtons & kEnterBit) == 0)
            {
                continue; // Enter went down after the grab: the user's real input
            }
            runtime.value        = 0.0F;
            runtime.heldDownSecs = 0.0F;
            ++keys;
        }
        else if (device == RE::INPUT_DEVICE::kMouse)
        {
            const auto idCode = button->GetIDCode();
            if (idCode > 4 || (heldButtons & (kMouseBit << idCode)) == 0)
            {
                continue; // not held at the grab (or a wheel pseudo-button)
            }
            runtime.value        = 0.0F;
            runtime.heldDownSecs = 0.0F;
            ++clicks;
        }
    }
    if (keys != 0 || clicks != 0)
    {
        // Info level on purpose: in-game this is the evidence that the replays of
        // our own focus grab were caught, which is what keeps a numeric field
        // alive after the IME took the focus.
        logger::info(
            "Scrubbed {} replayed key press(es) and {} replayed mouse press(es) right after the focus grab ({} ms after it)",
            keys,
            clicks,
            elapsedMs);
    }
}

void DispatchInputEventThunk(RE::BSTEventSource<RE::InputEvent *> *a_dispatcher, RE::InputEvent **a_events)
{
    // The framework bridge's filter runs first: it counts this batch's presses and
    // zeroes the composition ASCII echoes for its own sessions.
    SkseMenuFrameworkBridge::NeutralizeRawAscii(a_events);
    NeutralizeCompositionCommitKey(a_events);
    SuppressFocusGrabReplays(a_events);
    s_inputHook->Original(a_dispatcher, a_events);
}

} // namespace

namespace
{
auto IsPhysicallyDown(const int a_vk) -> bool
{
    return (::GetAsyncKeyState(a_vk) & 0x8000) != 0;
}
} // namespace

auto InputDispatchHook::FocusGrabBlockerDown() -> bool
{
    // Pointer buttons: the press that opened the field in the first place, and the
    // drag a Slider/Drag is usually doing. Enter: the key that opens a numeric
    // field and validates an InputText. Modifiers are deliberately absent — their
    // replay cannot deactivate a field, and Ctrl must stay usable for the
    // Ctrl+click that opens one.
    return IsPhysicallyDown(VK_LBUTTON) || IsPhysicallyDown(VK_RBUTTON) || IsPhysicallyDown(VK_MBUTTON) ||
           IsPhysicallyDown(VK_RETURN);
}

void InputDispatchHook::NoteFocusMoved()
{
    s_focusGrabMs.store(NowMs(), std::memory_order_relaxed);
    // Snapshot the physically held buttons so the scrubber can tell a replay
    // apart from a click that starts after the grab. GetAsyncKeyState reads
    // the physical state regardless of which window holds the focus.
    std::uint32_t held = 0;
    if (IsPhysicallyDown(VK_RETURN))
    {
        held |= kEnterBit;
    }
    const int mouseButtons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        if (IsPhysicallyDown(mouseButtons[i]))
        {
            held |= kMouseBit << i;
        }
    }
    s_focusGrabHeldButtons.store(held, std::memory_order_relaxed);
}

void InputDispatchHook::Install()
{
    if (s_installAttempted.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    s_inputHook = std::make_unique<DispatchInputEventHookData>(&DispatchInputEventThunk);
    if (s_inputHook->GetAddress() == 0)
    {
        logger::error(
            "Input dispatch hook resolved to address 0; a composition's Enter reaches the self-drawn hosts unfiltered");
    }
}
} // namespace Hooks
