Added
- Focus-grab deferral. Moving the Win32 focus to our IME window is now held back
  (up to 600 ms) while the OS still reports a pointer button or Enter physically
  down, and it is retried by the game thread's frame poll. The focus move makes
  the game window's WndProc clear ImGui's key/mouse state while the engine keeps
  polling the physical devices, so the still-held button came back as a *fresh*
  press: a replayed Enter validated the InputText and a replayed mouse press
  counted as "clicked outside the field", which is why the temporary numeric
  field a Drag/Slider opens died in the frame it appeared.
- One shared input-dispatch detour (`Hooks::InputDispatchHook`) on the call that
  hands a polled batch from `BSInputDeviceManager::PollInputDevices` to
  `BSTEventSource<InputEvent*>::SendEvent`. It is installed for every self-drawn
  ImGui host — SKSE Menu Framework, FLICK/FUCK, stb_widgets — not just for the
  framework bridge, and it scrubs the replays of our own focus moves (in both
  directions) for a short window after each one.
- An ImGui text session now requires the host's `ActiveId` to be non-zero, and a
  session outlives a single-frame `WantTextInput` by 250 ms. A host reports the
  want for exactly one frame in the shapes that open a numeric field; ending the
  session on that frame released the text-entry lease and disabled the IME one
  frame after it had been enabled.

Fixed
- Enter or Escape during a composition no longer throws away the composition and
  the field: in SKSE Menu Framework and FLICK menus the value field stopped
  flashing into existence and never accepting input, and the pinyin now commits
  instead of vanishing.
- A deferred IME enable is dropped when the ImGui text session that asked for it
  already ended, instead of lighting the candidate bar for a millisecond.
- Enter with a composition up but no candidate list reaches the TIP through the
  TSF keystroke channel. The engine stream carries no copy any more (a
  composition's Enter is zeroed for the self-drawn hosts), so dropping it here
  left the pinyin sitting there with nothing to commit it.
- Keys reach the IME while composing even when the keyboard mute is not in force,
  i.e. when the collapse did not land for that key.

Notes
- The 250 ms hangover means a click on a value field keeps the focus for up to a
  quarter of a second even if you click straight back out. It is what gives the
  lease and the IME time to come up; without it the field never opened at all.
- The deferral reads the held button from `GetAsyncKeyState` rather than the
  engine's input stream on purpose: once our IME window owns the focus, DirectInput
  stops reporting releases, so the engine's held-key set goes stale and never
  drains.
