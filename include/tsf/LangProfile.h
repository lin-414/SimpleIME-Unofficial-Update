//
// Created by jamie on 2026/2/6.
//

#pragma once

#include <msctf.h>
#include <string>
#include <winnt.h>

template <>
struct std::hash<GUID>
{
    auto operator()(const GUID &guid) const noexcept -> std::size_t
    {
        std::size_t const h1            = std::hash<uint32_t>()(guid.Data1);
        std::size_t const h2            = std::hash<uint16_t>()(guid.Data2);
        std::size_t const h3            = std::hash<uint16_t>()(guid.Data3);
        uint64_t          data4Combined = 0;
        std::memcpy(&data4Combined, guid.Data4, sizeof(data4Combined));
        std::size_t const h4 = std::hash<uint64_t>()(data4Combined);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};

namespace Ime
{
constexpr auto        LANGID_ENG         = 0x409;
constexpr std::size_t MAX_GUID_CHAR_SIZE = 64;

struct LangProfile
{
    std::string localeDisplayName; ///< e.g. 简体中文(中国大陆)
    std::string desc;              ///< e.g. 微软拼音
    std::string language;          ///< e.g. ENG, 简体中文
    CLSID       clsid{};
    GUID        guidProfile{};
    LANGID      langid{};
    DWORD       dwProfileType{};
    HKL         hkl{};
};

// Display-only stub (returned by GetActiveLangProfile() when nothing is cached).
// NEVER an activation target: a KEYBOARDLAYOUT profile needs a real HKL, and
// CLSID_NULL/GUID_NULL match no actual TSF profile. ActivateKeyboardEng() builds
// the real profile from the TSF enumeration instead.
inline const auto DEFAULT_LANG_PROFILE = LangProfile{"English(UK)", "ENG", "English", CLSID_NULL, GUID_NULL, LANGID_ENG, TF_PROFILETYPE_KEYBOARDLAYOUT, 0};

inline auto ToStringFromGUID2(const GUID &guid) -> std::wstring
{
    // StringFromGUID2 writes into a caller-owned buffer and returns the number of
    // characters written (including the terminating null), or 0 on failure. A
    // reserved-but-empty wstring has size() == 0, so passing it straight through
    // made the call a no-op and this function always returned an empty string.
    std::wstring wsGuid(MAX_GUID_CHAR_SIZE, L'\0');
    const int    written = StringFromGUID2(guid, wsGuid.data(), static_cast<int>(wsGuid.size()));
    if (written <= 0)
    {
        return {};
    }
    wsGuid.resize(static_cast<size_t>(written) - 1); // drop the terminating null
    return wsGuid;
}

} // namespace Ime
