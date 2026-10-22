#pragma once

namespace Hooks
{
/// Owns the single detour on the call that hands one polled input batch from
/// BSInputDeviceManager::PollInputDevices to BSTEventSource<InputEvent*>::SendEvent,
/// and runs every filter before any sink sees the batch.
///
/// This lives outside SkseMenuFrameworkBridge because the keys it has to zero —
/// Enter on a self-drawn ImGui field, and the replays of our own focus grab —
/// end the text session of *every* such host, including the ones that are not
/// SKSE Menu Framework at all (FLICK / FUCK draw their own ImGui from HUDMenu's
/// PostDisplay and read input from this same engine queue). Two detours on the
/// same call site would trampoline into each other, so the framework bridge's
/// ASCII echo filter goes through the same thunk.
class InputDispatchHook
{
public:
    /// Installs the detour once. Safe to call from the game thread at startup; a
    /// failed address resolve leaves every filter inert (pure passthrough).
    static void Install();

    /// Marks the moment we moved the Win32 focus — in EITHER direction: to our IME
    /// window when a session starts, and back to the game window when it ends.
    ///
    /// Both directions have the same side effect. The window that loses the focus
    /// clears its ImGui key/mouse state (WM_KILLFOCUS -> ClearInputKeys, and in
    /// some hosts ClearInputMouse too), while the engine keeps polling the physical
    /// devices and re-delivers whatever is still held as *fresh* presses. In the
    /// grabbing direction that kills a field that is opening (a replayed Enter
    /// validates it, a replayed mouse press counts as "clicked outside"). In the
    /// returning direction it leaves the host's ImGui believing a button is down
    /// that the user already released — and the NEXT click on a field then lands on
    /// an already-pressed mouse, which ImGui reads as "clicked outside the field"
    /// again. That is the in-game 2026-10-21 report: after committing with Enter, the
    /// very next Ctrl+click on a value field always flashed, and the one after it
    /// worked.
    ///
    /// That move makes the game window's WndProc run ImGui's ClearInputKeys() and
    /// ClearInputMouse(), while the engine keeps polling the physical devices: the
    /// next batches re-deliver whatever the user still holds as *fresh* presses. A
    /// replayed Enter validates an InputText and a replayed mouse press counts as
    /// "clicked outside the field", so the temporary numeric field a mod menu opens
    /// from a Drag/Slider dies in the frame it appeared. The dispatch thunk scrubs
    /// those replays for a short window after this call.
    ///
    /// Called from the IME thread; the thunk reads it on the game thread.
    static void NoteFocusMoved();

    /// True while the OS reports a pointer button or the confirm key physically
    /// down — the condition under which grabbing the focus now would replay that
    /// button into a host's ImGui field and close it.
    ///
    /// Read with GetAsyncKeyState, deliberately NOT from the engine's input
    /// stream: after our IME window owns the Win32 focus, DirectInput stops
    /// reporting releases, so the engine's held-key set goes stale and never
    /// drains (observed in-game 2026-10-21: stuck at 3-4 keys for a whole menu
    /// session). The OS keeps the physical state truthful no matter which window
    /// is focused, and it cannot get stuck.
    [[nodiscard]] static auto FocusGrabBlockerDown() -> bool;
};
} // namespace Hooks
