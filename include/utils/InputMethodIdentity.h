//
// Pure decision logic for the user-configured "default input method"
// ([core] default_input_method): encode/decode a stable identity for a TSF
// language profile and match it against the enumerated profile list.
//
// Header-only and free of Windows/SKSE dependencies so the headless suite
// covers it (same pattern as utils/LanguagePreference.h and
// utils/ConversionMemoryLogic.h).
//
// Why an identity string instead of a GUID: every keyboard layout shares
// guidProfile == GUID_NULL, so a GUID alone cannot tell two layouts apart (see
// InputMethodManager::ActivateProfile). HKL cannot be persisted either — it is a
// runtime handle whose high word is a USER-subsystem device/sort code (measured:
// LoadKeyboardLayout("00010409") -> 0xF0020409), and two different KLIDs can map
// onto the same HKL. The only stable key for a TIP is the triple TSF itself
// registers under: (clsid, guidProfile, langid) ->
// HKLM\SOFTWARE\Microsoft\CTF\TIP\{clsid}\LanguageProfile\0x<langid>\{<guid>}.
//
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Ime::InputMethodIdentity
{
/// Stored form of a TIP (input processor, e.g. Microsoft Pinyin / Sogou).
inline constexpr std::string_view kTipPrefix      = "tip:";
/// Stored form of a keyboard layout. These ARE selectable in the settings
/// window now; the KLID is resolved by InputMethodManager while enumerating the
/// installed layouts (HIWORD(hkl) is the KLID; a substitute layout is looked up
/// back through LoadKeyboardLayout).
inline constexpr std::string_view kKeyboardPrefix = "keyboard:";

struct Identity
{
    enum class Kind
    {
        Unset,   ///< nothing configured: do not touch the input method
        Unknown, ///< unparsable: treated like Unset, but logged
        Tip,
        Keyboard,
        Desc ///< free-form fallback: match the profile's display name verbatim
    };

    Kind          kind = Kind::Unset;
    std::string   clsid;      ///< "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
    std::string   profile;    ///< same shape; the TIP's guidProfile
    std::uint16_t langid = 0; ///< 0 = not constrained
    std::string   klid;       ///< "00000804"
    std::string   desc;       ///< display name (filled whenever known)
};

namespace Detail
{
[[nodiscard]] constexpr auto AsciiLower(const char c) -> char
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] inline auto SameText(const std::string_view lhs, const std::string_view rhs) -> bool
{
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (AsciiLower(lhs[i]) != AsciiLower(rhs[i])) return false;
    }
    return true;
}

[[nodiscard]] inline auto StartsWithNoCase(const std::string_view text, const std::string_view prefix) -> bool
{
    return text.size() >= prefix.size() && SameText(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] inline auto Trim(const std::string_view text) -> std::string_view
{
    const auto isBlank = [](const char c) -> bool {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && isBlank(text[begin]))
        ++begin;
    while (end > begin && isBlank(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

[[nodiscard]] inline auto IsHexDigit(const char c) -> bool
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" — the textual form
/// ToStringFromGUID2 produces. Validated so a typo in the TOML honestly reports
/// "unparsable" instead of silently matching nothing.
[[nodiscard]] inline auto IsGuidText(const std::string_view text) -> bool
{
    static constexpr std::size_t kDashes[4] = {9, 14, 19, 24}; // index inside the braces
    if (text.size() != 38) return false;
    if (text.front() != '{' || text.back() != '}') return false;

    std::size_t dashIndex = 0;
    for (std::size_t i = 1; i + 1 < text.size(); ++i)
    {
        if (dashIndex < 4 && i == kDashes[dashIndex])
        {
            if (text[i] != '-') return false;
            ++dashIndex;
            continue;
        }
        if (!IsHexDigit(text[i])) return false;
    }
    return dashIndex == 4;
}

[[nodiscard]] inline auto IsKlidText(const std::string_view text) -> bool
{
    if (text.size() != 8) return false;
    for (const char c : text)
    {
        if (!IsHexDigit(c)) return false;
    }
    return true;
}

[[nodiscard]] inline auto ParseHex16(const std::string_view text, std::uint16_t &out) -> bool
{
    if (text.empty() || text.size() > 4) return false;
    std::uint32_t value = 0;
    for (const char c : text)
    {
        if (!IsHexDigit(c)) return false;
        const std::uint32_t digit = (c <= '9') ? static_cast<std::uint32_t>(c - '0') : static_cast<std::uint32_t>(AsciiLower(c) - 'a' + 10);
        value                     = (value << 4) | digit;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}
} // namespace Detail

/// "", "auto", "system", "none" (any case, surrounding blanks ignored) mean
/// "leave the input method alone" — the shipped default.
[[nodiscard]] inline auto IsUnset(const std::string_view raw) -> bool
{
    const auto text = Detail::Trim(raw);
    return text.empty() || Detail::SameText(text, "auto") || Detail::SameText(text, "system") || Detail::SameText(text, "none");
}

/// tip:{CLSID}/{GUID}[/{langid, 1-4 hex digits}] — the langid suffix is optional
/// and only needed when one GUID is registered for several languages.
[[nodiscard]] inline auto FormatFromTip(const std::string_view clsid, const std::string_view profile, const std::uint16_t langid = 0) -> std::string
{
    std::string text{kTipPrefix};
    text += clsid;
    text += '/';
    text += profile;
    if (langid != 0)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        text += '/';
        text += kHex[(langid >> 12) & 0xF];
        text += kHex[(langid >> 8) & 0xF];
        text += kHex[(langid >> 4) & 0xF];
        text += kHex[langid & 0xF];
    }
    return text;
}

[[nodiscard]] inline auto FormatFromKeyboard(const std::string_view klid) -> std::string
{
    std::string text{kKeyboardPrefix};
    text += klid;
    return text;
}

/// Never throws. An unrecognised string comes back as Kind::Desc (match by
/// display name) so a hand-written name in the TOML still does something
/// sensible; a malformed tip:/keyboard: prefix comes back as Kind::Unknown.
[[nodiscard]] inline auto Parse(const std::string_view raw) -> Identity
{
    Identity   identity;
    const auto text = Detail::Trim(raw);
    if (IsUnset(text))
    {
        identity.kind = Identity::Kind::Unset;
        return identity;
    }

    if (Detail::StartsWithNoCase(text, kTipPrefix))
    {
        const auto body  = text.substr(kTipPrefix.size());
        const auto slash = body.find('/');
        if (slash == std::string_view::npos)
        {
            identity.kind = Identity::Kind::Unknown;
            return identity;
        }
        identity.clsid    = std::string{body.substr(0, slash)};
        const auto rest   = body.substr(slash + 1);
        const auto slash2 = rest.find('/');
        identity.profile  = std::string{rest.substr(0, slash2)};

        if (!Detail::IsGuidText(identity.clsid) || !Detail::IsGuidText(identity.profile))
        {
            identity.kind = Identity::Kind::Unknown;
            return identity;
        }
        if (slash2 != std::string_view::npos && !Detail::ParseHex16(rest.substr(slash2 + 1), identity.langid))
        {
            identity.kind = Identity::Kind::Unknown;
            return identity;
        }
        identity.kind = Identity::Kind::Tip;
        return identity;
    }

    if (Detail::StartsWithNoCase(text, kKeyboardPrefix))
    {
        const auto body = text.substr(kKeyboardPrefix.size());
        if (!Detail::IsKlidText(body))
        {
            identity.kind = Identity::Kind::Unknown;
            return identity;
        }
        identity.klid = std::string{body};
        identity.kind = Identity::Kind::Keyboard;
        return identity;
    }

    identity.kind = Identity::Kind::Desc;
    identity.desc = std::string{text};
    return identity;
}

/// True when the enumerated profile (candidate) is the configured input method.
[[nodiscard]] inline auto Matches(const Identity &configured, const Identity &candidate) -> bool
{
    using Kind = Identity::Kind;
    switch (configured.kind)
    {
        case Kind::Tip:
            return candidate.kind == Kind::Tip && Detail::SameText(configured.clsid, candidate.clsid) &&
                   Detail::SameText(configured.profile, candidate.profile) && (configured.langid == 0 || configured.langid == candidate.langid);
        case Kind::Keyboard:
            // A keyboard-layout candidate whose KLID could not be resolved must
            // never match: two unresolvable layouts both carry an empty klid, and
            // SameText("", "") would then report a match for any of them.
            return candidate.kind == Kind::Keyboard && !configured.klid.empty() && !candidate.klid.empty() &&
                   Detail::SameText(configured.klid, candidate.klid);
        case Kind::Desc:
            return !candidate.desc.empty() && Detail::SameText(configured.desc, candidate.desc);
        case Kind::Unset:
        case Kind::Unknown:
            break;
    }
    return false;
}

/// True when `candidate` names the same TSF registration that is live
/// (`active`): same clsid and profile GUID text AND the same language variant.
/// A multi-language TIP enumerates once per language with an IDENTICAL
/// (clsid, guidProfile) — the GUID alone cannot tell its zh variant from its
/// ja one, so an "already active" check that ignores the langid would skip the
/// TSF activation and silently keep the wrong variant composing text (see
/// InputMethodManager::ActivateProfile). A `candidate` langid of 0 is
/// unconstrained (the by-GUID lookup semantics) and matches any variant; the
/// `active` side is always concrete, so it constrains the comparison.
[[nodiscard]] inline auto SameTipVariant(const Identity &candidate, const Identity &active) -> bool
{
    return candidate.kind == Identity::Kind::Tip && active.kind == Identity::Kind::Tip &&
           Detail::SameText(candidate.clsid, active.clsid) && Detail::SameText(candidate.profile, active.profile) &&
           (candidate.langid == 0 || candidate.langid == active.langid);
}
} // namespace Ime::InputMethodIdentity
