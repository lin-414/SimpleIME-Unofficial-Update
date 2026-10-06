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
} // namespace Scaleform

} // namespace Hooks

#endif // HOOK_H
