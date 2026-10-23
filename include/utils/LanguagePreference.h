//
// Pure decision logic for "language = auto": map the OS UI locale onto a
// bundled translation name. Header-only and free of Windows/SKSE
// dependencies so the headless suite covers it.
//
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace Ime::LanguagePreference
{
/// Sentinel stored in appearance.language / [appearance] language. Kept in the
/// config VERBATIM: writing the resolved name back would freeze the choice at
/// the first launch of a machine whose system language later changes.
inline constexpr std::string_view kAutoLanguage = "auto";

struct LocaleEntry
{
    std::string_view subtag;   ///< primary language subtag of a Windows locale name
    std::string_view language; ///< must match translate_<language>.toml
};

inline constexpr std::array<LocaleEntry, 6> kLocaleTable{
    LocaleEntry{"zh", "chinese"}, LocaleEntry{"ja", "japanese"}, LocaleEntry{"ko", "korean"},
    LocaleEntry{"ru", "russian"}, LocaleEntry{"de", "german"},   LocaleEntry{"en", "english"},
};

constexpr auto AsciiLower(const char c) -> char
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// "zh-Hans-CN" -> "chinese". Empty when the primary subtag is not bundled.
[[nodiscard]] constexpr auto FromLocaleName(const std::string_view localeName) -> std::string_view
{
    const auto end    = localeName.find('-');
    const auto subtag = localeName.substr(0, end == std::string_view::npos ? localeName.size() : end);
    for (const auto &entry : kLocaleTable)
    {
        if (entry.subtag.size() != subtag.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < subtag.size(); ++i)
        {
            same = same && AsciiLower(subtag[i]) == entry.subtag[i];
        }
        if (same) return entry.language;
    }
    return {};
}

/// Resolve the configured value into the translation name to load.
/// Non-"auto" values pass through untouched (an unknown name keeps today's
/// behaviour: the loader's own english fallback decides).
[[nodiscard]] inline auto Resolve(
    const std::string_view configured, const std::string_view systemLocale, const std::string_view fallbackLanguage
) -> std::string
{
    if (configured != kAutoLanguage) return std::string{configured};
    const auto detected = FromLocaleName(systemLocale);
    return std::string{detected.empty() ? fallbackLanguage : detected};
}

// Platform binding (src/utils/LanguagePreference.cpp).
[[nodiscard]] auto GetSystemLocaleName() -> std::string;
/// Resolve @p configured and hot-swap the translator. Returns the name actually
/// requested (the loader's own english fallback still applies when that file is
/// missing). @p dir must be the same dir the settings combo scans.
auto LoadForConfigured(std::string_view configured, const std::filesystem::path &dir, std::string_view fallbackLanguage = "english") -> std::string;

} // namespace Ime::LanguagePreference
