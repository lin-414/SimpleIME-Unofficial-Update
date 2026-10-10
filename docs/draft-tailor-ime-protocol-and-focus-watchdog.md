# 已提交：给 heathbrownkeyworks/Tailor 的两条 issue

> 状态：**已于 2026-10-09 以 lin-414 账号提交**，正文与下列草稿一致（措辞按证据收紧过）。
> - [#2 Focus watchdog closes the whole UI on a single frame](https://github.com/heathbrownkeyworks/Tailor/issues/2) —— bug，优先级更高（影响所有玩家，不限中文用户）
> - [#3 Cooperation protocol for external IME helpers](https://github.com/heathbrownkeyworks/Tailor/issues/3) —— 功能请求（窗口属性握手协议）
>
> 依据：本机 SimpleIME + Tailor 3.0.1 实机验证（2026-10-09 通过）。
> 下面保留原始草稿全文，便于日后对照上游的回应改动。


---

## Issue 1 — Feature: let an in-game IME helper cooperate with text fields (window-property protocol)

**Title:** Public handshake for external IME helpers: drop preedit keystrokes while an IME composes

**Body:**

Tailor 3.x draws its UI with Dear ImGui fed from engine `RE::InputEvent`s
(`ImGuiHost::ProcessEvent`). That makes Chinese/Japanese input impossible for an
external helper: a helper cannot write into a private ImGui context it does not
own, and — worse — the engine generates `CharEvent`s from the physical keyboard
before the helper's IME ever sees the keystroke, so the raw pinyin lands in the
field next to the committed text (`fuzhuang服装`).

I maintain SimpleIME, an SKSE plugin that provides TSF/IME input in Skyrim menus.
A working protocol is implemented and tested; I am proposing it as a small,
optional addition on Tailor's side so any IME helper can use it, and so a future
Tailor release does not silently break it.

Both plugins run in the same process, so a **window property on the game's swapchain
`OutputWindow`** is enough — no shared memory, no new IPC:

- `SimpleIME.Composing` — present only while an IME composition is active, value is
  the last refreshed `GetTickCount64()`. Readers should treat it as active while it
  is younger than ~500 ms, which makes a helper that crashed mid-composition
  self-heal.
- `SimpleIME.ImeAware` — set once by the UI at ImGui init to declare "I drop
  preedit keystrokes myself; the helper must not delete characters afterwards".

Tailor side (this is the whole change; `s.window` is the value you already store
from `desc.OutputWindow`):

```cpp
// at ImGui init, right after `s.window = desc.OutputWindow;`
if (const auto aware = GlobalAddAtomW(L"SimpleIME.ImeAware"); aware != 0) {
    SetPropW(desc.OutputWindow, reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(aware)), reinterpret_cast<HANDLE>(1));
}

// in the kChar branch of ImGuiHost::ProcessEvent
if (event->eventType == RE::INPUT_EVENT_TYPE::kChar && HasFocus()) {
    const auto code = static_cast<const RE::CharEvent*>(event)->keyCode;
    if (input::IsTextCharacter(code)) {
        const bool composing = ExternalCompositionActive(s.window.load());
        if (code < 0x80) {
            if (composing) { sawComposition = true; continue; }   // preedit, not text
            batch.push_back({State::Input::Kind::Char, static_cast<int>(code)});
            if (!sawComposition) strayPreedit += 1;
            continue;
        }
        // a committed string from the helper: undo the racing first letter in the
        // same batch, so the field never shows it next to the result
        if (sawComposition && strayPreedit > 0) { /* push ImGuiKey_Backspace down+up */ }
        strayPreedit = 0; sawComposition = false;
        batch.push_back({State::Input::Kind::Char, static_cast<int>(code)});
    }
}
```

Two details that took real testing to find, and which are easy to get wrong:

1. **The first keystroke always wins the race.** Skyrim polls the input device
   *before* the message reaches the window proc, so the very first letter of a
   composition arrives as a `CharEvent` before any helper can publish its flag.
   Undoing exactly that one letter (capped at 1, and only when a composition was
   actually observed) is what makes the field clean without ever deleting text the
   player typed. Doing the undo inside the same `batch`/`NewFrame` is what makes it
   invisible; a helper that instead sends backspaces through the engine queue is
   limited to ~5 deletions per frame (`BSInputEventQueue` has 5 `charEvents` and 10
   `buttonEvents` slots) and visibly "deletes letter by letter".
2. **Only drop `code < 0x80`.** Committed CJK arrives as non-ASCII, so a stale flag
   can never eat the helper's own text — the failure mode is limited to "an English
   letter is ignored for <500 ms".

Happy to test any build against SimpleIME, and to write this up as a short doc for
other UI mods that draw with a private ImGui context.

---

## Issue 2 — Bug: the focus watchdog closes the whole UI on a single frame, so any pausing menu (or screenshot tool) kills Tailor

**Title:** `ImGuiHost::HasFocus()` closes the UI with no hysteresis — a one-frame foreground or pause change is fatal

**Body:**

`ImGuiHost::HasFocus()` (3.0.1) requires all of these, and it is polled from
`DrawFrame`, from `ProcessEvent` (i.e. on **every input batch**, including the batch
of the click itself) and from `TailorPreviewSession`:

```cpp
if (!s.active || !s.desired || (window && GetForegroundWindow() != window)) return false;
return ui && !ui->GameIsPaused() && ui->IsMenuOpen(TailorMenu::MENU_NAME);
```

Any single frame where one of them is false runs `QueueLifecycleClose()` →
`CloseForLifecycle(EndReason::FocusLost)` → the menu is hidden and does **not** come
back until the player re-opens it. There is no grace period and no distinction
between "the player really left Tailor" and "something transient happened". Two
consequences I reproduced with logging:

- **`!ui->GameIsPaused()` is not a liveness signal.** `GameIsPaused()` is
  `numPausesGame > 0`, i.e. "some menu on the stack has `kPausesGame`". Any helper
  mod that legitimately pauses the game — a settings overlay, an MCM-style window —
  closes Tailor instantly and permanently. Observed: `numPausesGame 0 → 1` and
  `tailorOpen true → false` in the same frame, with no player action at all.
- **`GetForegroundWindow()` is trivially transient.** Observed: a capture tool's Qt
  tool window (`class[Qt51513QWindowToolSaveBits]`, different pid) took the
  foreground, and 21 ms later Tailor closed itself. Alt-Tab, a notification, an
  on-screen-keyboard or IME candidate window all look the same.

Suggested direction (any of these removes the false positives, none of them changes
the intended behaviour):

1. Require N consecutive failing polls (e.g. 10 frames, or ~250 ms) before closing,
   instead of one; and/or
2. drop `!GameIsPaused()` from the liveness test — a paused game is not a closed
   UI — or only honour it when Tailor's own menu is off the stack; and/or
3. treat foreground loss as "stop consuming input" rather than "close", and let the
   normal `kHide`/hotkey paths do the actual closing.

Worth noting: `EndReason::FocusLost` is also what `QueueLifecycleClose()` reports for
unrelated failures (an action throwing, `facadeReady` going false, an ImGui draw
error), so from the log alone a player cannot tell "I lost focus" from "something
broke". A distinct reason per call site would make these reports much easier to
diagnose.
