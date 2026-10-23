#include "utils/LanguagePreference.h"

#include "i18n/translator_manager.h" // extern/JamieMods/common
#include "log.h"
#include "WCharUtils.h"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace Ime::LanguagePreference
{
namespace
{
/// The user's preferred UI language as a BCP-47 name ("zh-Hans-CN").
/// GetUserPreferredUILanguages is what Windows renders its own UI in; a
/// multi-language install can prefer a language GetSystemDefaultUILanguage
/// does not report. LCIDToLocaleName is the fallback for the rare case where
/// the MUI list is unavailable.
auto QuerySystemLocaleName() -> std::string
{
    ULONG count = 0;
    ULONG chars = 0;
    if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars) && chars > 0)
    {
        std::wstring buffer(chars, L'\0');
        if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &chars))
        {
            // NUL-separated list, double-NUL terminated; the head is preferred.
            if (const auto nul = buffer.find(L'\0'); nul != std::wstring::npos) buffer.resize(nul);
            if (!buffer.empty()) return WCharUtils::ToString(buffer);
        }
    }
    const LANGID langid = ::GetUserDefaultUILanguage();
    if (langid == 0) return {};
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (::LCIDToLocaleName(MAKELCID(langid, SORT_DEFAULT), localeName, LOCALE_NAME_MAX_LENGTH, 0) > 0)
    {
        return WCharUtils::ToString(localeName);
    }
    return {};
}
} // namespace

auto GetSystemLocaleName() -> std::string
{
    // The OS UI language cannot change under a running game in any way we
    // observe; one query per process is enough and keeps the render thread off
    // the registry.
    static const std::string cached = QuerySystemLocaleName();
    return cached;
}

auto LoadForConfigured(const std::string_view configured, const std::filesystem::path &dir, const std::string_view fallbackLanguage) -> std::string
{
    const auto resolved = Resolve(configured, GetSystemLocaleName(), fallbackLanguage);
    logger::info(
        "i18n: UI language '{}' resolved to '{}' (system locale '{}')", configured, resolved, GetSystemLocaleName()
    );
    i18n::UpdateTranslator(resolved, fallbackLanguage, dir);
    return resolved;
}
} // namespace Ime::LanguagePreference
