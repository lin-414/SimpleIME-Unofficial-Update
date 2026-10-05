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
} // namespace Scaleform

} // namespace Hooks

#endif // HOOK_H
