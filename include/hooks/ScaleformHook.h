//
// Created by jamie on 2025/3/2.
//

#ifndef HOOK_H
#define HOOK_H

#include <cstdint>

namespace Hooks
{
namespace Scaleform
{
void Install();
void Uninstall();
/// Re-sync the hook's cached text-entry count from the game after the counter
/// was corrected elsewhere (leak repair) — otherwise the next 0->1 transition
/// would be misdetected.
void ResetTextEntryCountCache();
/// Game thread, every frame: commit an IME enable that landed inside the
/// re-enable debounce window and was deferred (not dropped). The final stable
/// state wins: still-open counter -> enable; closed -> nothing.
void CommitPendingTextEntryEnable();
/// Whether the IME enable in flight has to wait before it moves the Win32 focus to
/// our IME window. Moving the focus replays whatever the user still holds into the
/// host's ImGui field (Enter validates it, a mouse press counts as "clicked
/// outside"), which closes the temporary numeric field a Drag/Slider just opened.
/// The held state comes from the OS, never from the engine's input stream.
[[nodiscard]] auto ShouldDeferEnableForFocusGrab() -> bool;
/// Latch the currently requested IME enable until the OS reports the button
/// released. The game thread's frame poll (CommitPendingTextEntryEnable) commits it.
void DeferTextEntryEnableForFocusGrab();
/// Consume the one-shot release the frame poll arms when the deferral ceiling
/// expires, so the resuming enable is not deferred again by its own guard.
[[nodiscard]] auto ConsumeFocusGrabDeferBypass() -> bool;
} // namespace Scaleform

} // namespace Hooks

#endif // HOOK_H
