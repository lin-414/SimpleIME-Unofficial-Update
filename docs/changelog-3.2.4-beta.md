Added
- Suppress Game Input While Typing (Settings → Behaviour → Input, on by default).
  While an IME composition owns the keyboard the plugin collapses the keyboard
  device's polled key state (BSWin32KeyboardDevice::Poll's vtable slot), so the
  engine generates no keyboard InputEvent and other plugins' hotkeys —
  PgUp/PgDn and friends — cannot fire mid-typing. The event source keeps
  running, so the mouse and gamepad still move, click and scroll the candidate
  window.
- Keyboard forwarding into the IME. While the engine's keyboard stream is muted,
  the keys the IME edits with — PageUp/PageDown (page the candidate list),
  Up/Down (move the highlight), Left/Right (move the caret), Backspace (delete
  a pinyin letter), Enter/Space (commit) — are handed to the TIP through the
  TSF keystroke channel, because the IME window disables its own keyboard
  channel by design while a composition is active.

Fixed
- Suppression now tracks the composition (IN_COMPOSING) instead of lingering
  while a text entry merely has focus: after committing, Backspace and the
  console's exit key work again. A cached candidate UI element no longer keeps
  the keyboard muted after the candidate list closes.
- Only the keyboard is muted. An earlier revision skipped
  BSInputDeviceManager::PollInputDevices wholesale, which also froze the mouse
  and made the candidate window impossible to click or scroll.
- The suppression query reports "installed and active" from the hook install
  state rather than a detour flag, so a failed install can no longer leave the
  forwarding path believing the keyboard is being muted.
- The Russian translation gained the two new settings strings.

Notes
- The suppression redirects the keyboard device's polled state, relying on the
  engine diffing that state after Poll. A runtime that generated keyboard
  events from the DirectInput buffer instead would turn the feature inert; the
  keyboard forwarding path keeps the IME working either way.
- While composing, PgUp/PgDn page the candidate list and do not reach other
  mods; a mod that used those keys for something else simply sees them only
  outside composition.
