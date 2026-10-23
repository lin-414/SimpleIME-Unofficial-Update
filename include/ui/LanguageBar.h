//
// Created by jamie on 2026/2/6.
//

#pragma once

#include <cstdint>
#include <vector>

namespace Ime
{
struct LangProfile;

namespace UI
{
class ToolWindow;

namespace LanguageBar
{
/// @param activeProfileIndex index of the active profile inside @c langProfiles
///        (UINT32_MAX when unknown). An index, not a GUID: keyboard layouts all
///        share GUID_NULL, so a GUID cannot tell two of them apart.
auto Draw(
    bool &                       pinned,
    bool &                       toolWindowShowing,
    const LangProfile &          activeLangProfile,
    const std::vector<LangProfile> &langProfiles,
    std::uint32_t                activeProfileIndex,
    bool                         interactive
) -> void;
}; // namespace LanguageBar
} // namespace UI

} // namespace Ime
