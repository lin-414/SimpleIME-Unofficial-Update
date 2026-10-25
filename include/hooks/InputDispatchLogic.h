#pragma once

// Pure, SKSE-free decision logic of the engine input-dispatch hook, extracted so
// it can be unit-tested headless (the .cpp keeps all the RE/Windows interaction).
// Same split MeridianBridgeLogic.h / SkseMenuFrameworkBridgeLogic.h use.
namespace Hooks::InputDispatchLogic
{
/// Whether this input batch's confirm key (Enter) has to be zeroed before the
/// engine hands the batch to its sinks.
///
/// A host that draws its own ImGui field takes the confirm key straight from the
/// engine stream; its InputText then validates and deactivates. The commit that
/// key was meant to trigger has nowhere left to land — the field is gone, we
/// release the text-entry lease with it, and the pinyin is discarded. Zeroing the
/// key keeps the field alive for the commit the IME delivers instead.
///
/// Deliberately limited to self-drawn ImGui targets: on a Scaleform text field or
/// in the console Enter is the user's "execute" key and must reach the game.
///
/// param imeComposing      State::IN_COMPOSING — the same window in which
///                          InputSuppression mutes the keyboard, i.e. the window
///                          in which the key belongs to the IME.
/// param imeDisabled       State::IME_DISABLED — the TIP is off; the key is the
///                          user's again even if a stale composing flag lingers.
/// param imGuiHostTargeted the caller's resolved "a self-drawn ImGui surface owns
///                          this text input" verdict.
[[nodiscard]] inline auto ShouldSwallowCommitKey(
    const bool imeComposing,
    const bool imeDisabled,
    const bool imGuiHostTargeted) -> bool
{
    return imeComposing && !imeDisabled && imGuiHostTargeted;
}

/// Whether an IME enable has to wait before it moves the Win32 focus to our IME
/// window.
///
/// The wait exists because the focus move itself is what breaks a field that is
/// opening right now: the game window loses the focus, the hosting plugin's
/// WndProc clears the ImGui key/mouse state, and the engine's next poll re-delivers
/// whatever is still held as *fresh* presses — a replayed Enter validates the
/// InputText, a replayed mouse press counts as "clicked outside the field". Both
/// close the temporary numeric field a mod menu opens from a Drag/Slider in the
/// frame it appeared (in-game 2026-10-21: the first Ctrl+click on a value field
/// only flashed, the second worked).
///
/// The caller must ask the OS (GetAsyncKeyState), not the engine's input stream:
/// once the IME window owns the Win32 focus, DirectInput stops reporting releases,
/// so the engine's held-key set never drains (measured in-game: stuck at 3-4 keys
/// through a whole menu session, which made an engine-state gate churn forever).
///
/// param buttonDown  the OS reports a pointer button or Enter physically down.
/// param deferredForMs how long this enable has already been deferred.
/// param maxDeferMs  ceiling, so a long drag cannot keep the IME off for good.
[[nodiscard]] inline auto ShouldDeferFocusGrab(
    const bool buttonDown,
    const int deferredForMs,
    const int maxDeferMs) -> bool
{
    return buttonDown && deferredForMs < maxDeferMs;
}

/// Whether the inputs in this batch are the *replays* of a focus grab.
///
/// Moving the Win32 focus to our IME window makes the game window's WndProc call
/// ImGui's ClearInputKeys() and ClearInputMouse(). The engine keeps polling the
/// physical devices, so the very next batches re-deliver the buttons the user is
/// still holding as *fresh* presses. Two of those replays end an InputText that
/// is alive at that moment:
///   * a replayed Enter -> "validate" (the temporary numeric field a mod menu
///     opens from a Drag/Slider closes in the frame it appeared);
///   * a replayed mouse press -> "clicked outside the field" (InputTextEx
///     deactivates on io.MouseClicked when it did not itself just activate).
/// Both are pure side effects of our own focus grab, so the batch right after it
/// is scrubbed — but only for buttons that were physically held at the grab
/// (the caller snapshots them); input that begins after the grab flows through.
///
/// param msSinceFocusGrab elapsed time since our last focus grab, < 0 if none.
/// param windowMs         how long the scrubbing lasts after that grab.
[[nodiscard]] inline auto IsFocusGrabReplay(const int msSinceFocusGrab, const int windowMs) -> bool
{
    return msSinceFocusGrab >= 0 && msSinceFocusGrab < windowMs;
}
} // namespace Hooks::InputDispatchLogic
