#pragma once

namespace Hooks
{
/// Suppresses the engine's own keyboard InputEvents while a composition owns the
/// keyboard, so PgUp/PgDn and other SKSE plugins' hotkeys cannot fire mid-typing.
/// Stands in for the external "Typing Mode" mod: the IME reads keys from ImeWnd's
/// Win32 focus, not from this stream, so typing keeps working while muted.
///
/// Only the keyboard is muted: the keyboard device's polled key state is
/// collapsed right after its Poll, while BSInputDeviceManager::PollInputDevices
/// keeps running for the mouse and gamepad. The first iteration skipped
/// PollInputDevices wholly and thereby also froze the mouse, which stopped the
/// candidate window from being clicked or scrolled.
class InputSuppression
{
public:
    /// Installs the keyboard-device vtable hook. Safe to call from the game
    /// thread at startup; a failed install leaves suppression permanently off
    /// (IsSuppressing() then reports false).
    static void Install();

    /// Restores the original vtable slot.
    static void Uninstall();

    /// True while the keyboard poll is being collapsed.
    [[nodiscard]] static auto IsSuppressing() -> bool;
};
} // namespace Hooks
