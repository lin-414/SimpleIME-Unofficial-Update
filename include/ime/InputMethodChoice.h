//
// One entry of the settings-window "default input method" picker, plus the pure
// logic that decides which entry a stored [core] default_input_method selects.
//
// Header-only and free of Windows/SKSE dependencies so the headless suite covers
// the matching rules (same pattern as utils/InputMethodIdentity.h).
//
#pragma once

#include "utils/InputMethodIdentity.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Ime
{
struct InputMethodChoice
{
    /// Value stored in [core] default_input_method when this entry is picked.
    std::string identity; // tip:{CLSID}/{GUID}/{langid}
    /// Display name shown in the combo (e.g. the TIP's localized description).
    std::string name;
};

namespace InputMethodChoiceDetail
{
/// ASCII-only case folding, so UTF-8 names pass through untouched.
[[nodiscard]] inline auto ToLowerText(const std::string_view text) -> std::string
{
    std::string lowered{text};
    for (auto &c : lowered)
    {
        c = InputMethodIdentity::Detail::AsciiLower(c);
    }
    return lowered;
}

[[nodiscard]] inline auto ContainsNoCase(const std::string_view haystack, const std::string_view needle) -> bool
{
    if (needle.empty()) return false;
    return ToLowerText(haystack).find(ToLowerText(needle)) != std::string::npos;
}

[[nodiscard]] inline auto ParseChoice(const InputMethodChoice &choice) -> InputMethodIdentity::Identity
{
    auto identity = InputMethodIdentity::Parse(choice.identity);
    identity.desc = choice.name;
    return identity;
}
} // namespace InputMethodChoiceDetail

/// Index of the entry the configured value selects, or npos when it selects
/// nothing (unset, or not installed in this process).
///
/// Mirrors the two-pass match in InputMethodManager::ActivateConfiguredProfile:
/// an exact tip: identity first, then a case-insensitive display-name substring
/// so a hand-written name in the TOML still highlights its entry.
[[nodiscard]] inline auto FindChoiceIndex(const std::vector<InputMethodChoice> &choices, const std::string &configured) -> std::size_t
{
    namespace Detail  = InputMethodChoiceDetail;
    const auto target = InputMethodIdentity::Parse(configured);
    using Kind        = InputMethodIdentity::Identity::Kind;
    if (target.kind == Kind::Unset || target.kind == Kind::Unknown || target.kind == Kind::Keyboard) return std::string::npos;

    for (std::size_t i = 0; i < choices.size(); ++i)
    {
        if (InputMethodIdentity::Matches(target, Detail::ParseChoice(choices[i]))) return i;
    }

    if (target.kind == Kind::Desc)
    {
        const auto wanted = InputMethodIdentity::Detail::Trim(target.desc);
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            if (Detail::ContainsNoCase(choices[i].name, wanted)) return i;
        }
    }
    return std::string::npos;
}

/// Label for the entry at `index`: its display name, with the profile language
/// appended when another entry in the list shares that name — Microsoft ships
/// the same localized name (e.g. the CJK "Microsoft IME") for zh/ja/ko, so the
/// bare name would be ambiguous in the combo.
[[nodiscard]] inline auto ChoiceLabel(const std::vector<InputMethodChoice> &choices, const std::size_t index) -> std::string
{
    namespace Detail = InputMethodChoiceDetail;
    if (index >= choices.size()) return {};

    std::string label = choices[index].name;
    if (label.empty()) label = choices[index].identity;

    std::size_t sameName = 0;
    for (const auto &choice : choices)
    {
        if (InputMethodIdentity::Detail::SameText(choice.name, choices[index].name)) ++sameName;
    }
    if (sameName < 2) return label;

    const auto identity = Detail::ParseChoice(choices[index]);
    if (identity.langid == 0) return label;

    static constexpr char kHex[] = "0123456789ABCDEF";
    label += " (";
    for (int shift = 12; shift >= 0; shift -= 4)
    {
        label += kHex[(identity.langid >> shift) & 0xF];
    }
    label += ')';
    return label;
}
} // namespace Ime
