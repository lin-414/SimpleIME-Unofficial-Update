#include "tsf/InputMethodManager.h"

#include "WCharUtils.h"
#include "core/State.h"
#include "imguiex/ErrorNotifier.h"
#include "log.h"
#include "utils/InputMethodIdentity.h"

#include <Shlwapi.h>
#include <algorithm>
#include <future>
#include <msctf.h>
#include <stdexcept>

#pragma comment(lib, "Shlwapi.lib")

namespace
{
auto GetProfileCachedIndex(const std::vector<Ime::LangProfile> &langProfiles, const GUID &guidProfile, LANGID langid = 0, HKL hkl = nullptr) -> std::uint32_t
{
    if (guidProfile != GUID_NULL)
    {
        // Real TIPs (input processors) carry unique profile GUIDs — except that
        // a multi-language TIP registers the SAME guidProfile once per language
        // (see GetSelectableChoices), so a GUID-only lookup lands on the first
        // variant no matter which one is live. Resolve the exact (guidProfile,
        // langid) registration when the live langid is known (0 = unconstrained,
        // the legacy by-GUID semantics); with no exact hit, a sole same-GUID
        // entry is still unambiguous, several are not — "unknown" beats
        // guessing the wrong variant.
        std::uint32_t byGuidOnly = UINT32_MAX;
        bool          ambiguous  = false;
        for (std::size_t i = 0; i < langProfiles.size(); ++i)
        {
            const auto &profile = langProfiles[i];
            if (profile.guidProfile != guidProfile) continue;
            if (langid != 0 && profile.langid == langid) return static_cast<std::uint32_t>(i);
            if (byGuidOnly == UINT32_MAX)
                byGuidOnly = static_cast<std::uint32_t>(i);
            else
                ambiguous = true;
        }
        if (langid == 0) return byGuidOnly;
        return (!ambiguous && byGuidOnly != UINT32_MAX) ? byGuidOnly : UINT32_MAX;
    }
    // Keyboard-layout profiles all share guidProfile == GUID_NULL; matching by
    // GUID alone would always hit the first layout in the list regardless of
    // which layout is actually active. Disambiguate by the layout handle.
    const auto it = std::ranges::find_if(langProfiles, [&](const auto &p) -> bool {
        return p.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT && p.hkl == hkl;
    });
    if (it != langProfiles.end())
    {
        return static_cast<std::uint32_t>(std::ranges::distance(langProfiles.begin(), it));
    }
    return UINT32_MAX;
}

/// "00000409" for 0x409 — the KLID text form InputMethodIdentity stores.
auto FormatKlid(const std::uint32_t id) -> std::string
{
    return std::format("{:08X}", id);
}

auto ToWideKlid(const std::string_view klid) -> std::wstring
{
    std::wstring wide{klid.begin(), klid.end()};
    for (auto &c : wide)
    {
        if (c >= L'a' && c <= L'f') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return wide;
}

/// A KLID is only usable when the layout is registered on this machine.
auto IsRegisteredKeyboardLayout(const std::string &klid) -> bool
{
    const auto path = std::format(L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\{}", ToWideKlid(klid));

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    RegCloseKey(hKey);
    return true;
}

/// Slow path for layouts whose HIWORD(HKL) is not their KLID (see ResolveKlid):
/// walk the registered KLIDs, load each and keep the one whose freshly loaded
/// HKL matches. Only substitute layouts (e.g. 00010409 -> 0xF0020409) get here.
auto FindKlidByLoading(const HKL target) -> std::string
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return {};
    }

    std::string found;
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[64]  = {};
        DWORD   nameChars = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
        if (RegEnumKeyExW(hKey, index, name, &nameChars, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::string klid = WCharUtils::ToString(std::wstring_view{name, nameChars});
        if (!Ime::InputMethodIdentity::Detail::IsKlidText(klid)) continue;

        const auto wideKlid = ToWideKlid(klid);
        if (LoadKeyboardLayoutW(wideKlid.c_str(), 0) == target)
        {
            found = klid;
            break;
        }
    }
    RegCloseKey(hKey);
    return found;
}

/// The KLID of an HKL. HIWORD(hkl) IS the KLID for an ordinary layout
/// (measured: 00000409 -> 0x04090409), but a SUBSTITUTE layout puts a
/// USER-subsystem code there instead (measured: 00010409 -> 0xF0020409, and no
/// such registry key exists), so that case is resolved by loading the registered
/// layouts and comparing handles.
auto ResolveKlid(const HKL hkl) -> std::string
{
    if (hkl == nullptr) return {};

    const auto fromHighWord = FormatKlid(static_cast<std::uint32_t>(HandleToUlong(hkl)) >> 16);
    if (IsRegisteredKeyboardLayout(fromHighWord)) return fromHighWord;

    auto resolved = FindKlidByLoading(hkl);
    if (!resolved.empty()) return resolved;
    // Unresolvable: keep the raw value for the log, but it names no installed
    // layout — Matches() requires a non-empty klid, and this one matches nothing
    // the user can store unless the layout really is registered under it.
    return fromHighWord;
}

/// Stable identity of an enumerated profile, for matching against the user's
/// configured default input method (utils/InputMethodIdentity.h). A TIP is
/// identified by (clsid, guidProfile, langid) — the triple TSF itself registers
/// under. A keyboard layout is identified by its KLID: all layouts share
/// GUID_NULL, and the KLID (not the HKL, a per-session handle) is what survives
/// a restart.
auto ToIdentity(const Ime::LangProfile &langProfile) -> Ime::InputMethodIdentity::Identity
{
    using Kind = Ime::InputMethodIdentity::Identity::Kind;

    Ime::InputMethodIdentity::Identity identity;
    identity.desc = langProfile.desc;
    if (langProfile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
    {
        identity.kind    = Kind::Tip;
        identity.clsid   = WCharUtils::ToString(Ime::ToStringFromGUID2(langProfile.clsid));
        identity.profile = WCharUtils::ToString(Ime::ToStringFromGUID2(langProfile.guidProfile));
        identity.langid  = langProfile.langid;
        return identity;
    }
    identity.kind = Kind::Keyboard;
    identity.klid = ResolveKlid(langProfile.hkl);
    return identity;
}


/// Case-insensitive helpers for the lenient display-name match below. Only
/// ASCII is folded, so UTF-8 sequences (中文名) pass through untouched.
auto ToLowerText(const std::string_view text) -> std::string
{
    std::string lowered{text};
    for (auto &c : lowered)
    {
        c = Ime::InputMethodIdentity::Detail::AsciiLower(c);
    }
    return lowered;
}

auto ContainsNoCase(const std::string_view haystack, const std::string_view needle) -> bool
{
    if (needle.empty()) return false;
    return ToLowerText(haystack).find(ToLowerText(needle)) != std::string::npos;
}

template <std::size_t SIZE>
using WStringBuffer = std::array<wchar_t, SIZE>;

auto GetKeyboardLayoutDisplayName(HKL hkl) -> std::string
{
    constexpr size_t maxLayoutDisplayNameSize = 256;

    const auto keyboardRegPath = std::format(L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\{:08x}", HandleToUlong(hkl) >> 16);

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyboardRegPath.data(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return {};
    }

    std::string result;
    DWORD       bufferBytes = 0;
    if (RegQueryValueExW(hKey, L"Layout Display Name", nullptr, nullptr, nullptr, &bufferBytes) == ERROR_SUCCESS)
    {
        std::vector<wchar_t> displayNameBuf(static_cast<size_t>(bufferBytes) / sizeof(wchar_t));
        if (RegQueryValueExW(hKey, L"Layout Display Name", nullptr, nullptr, reinterpret_cast<LPBYTE>(displayNameBuf.data()), &bufferBytes) ==
            ERROR_SUCCESS)
        {
            WStringBuffer<maxLayoutDisplayNameSize> resolvedName = {};
            if (SUCCEEDED(SHLoadIndirectString(displayNameBuf.data(), resolvedName.data(), resolvedName.size(), nullptr)))
            {
                result = WCharUtils::ToString(std::wstring_view(resolvedName.data()));
            }
        }
    }
    RegCloseKey(hKey);
    return result;
}

auto GetLangProfileDesc(ITfInputProcessorProfiles *processorProfiles, const TF_INPUTPROCESSORPROFILE &profile) -> std::string
{
    if (profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT && profile.hkl != nullptr)
    {
        return GetKeyboardLayoutDisplayName(profile.hkl);
    }

    CComBSTR bStrDesc = nullptr;
    if (SUCCEEDED(processorProfiles->GetLanguageProfileDescription(profile.clsid, profile.langid, profile.guidProfile, &bStrDesc)))
    {
        const std::wstring_view wsvDesc(bStrDesc, bStrDesc.Length());
        return WCharUtils::ToString(wsvDesc);
    }
    return {};
}

auto GetLocaleName(LANGID langid) -> std::wstring
{
    const int len = LCIDToLocaleName(MAKELCID(langid, SORT_DEFAULT), nullptr, 0, 0);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len), L'\0');
    if (LCIDToLocaleName(MAKELCID(langid, SORT_DEFAULT), buf.data(), len, 0) <= 0) return {};
    buf.resize(static_cast<size_t>(len) - 1); // strip trailing null
    return buf;
}

auto GetLocaleInfo(std::wstring_view localeName, LCTYPE LCType) -> std::wstring
{
    const auto infoLen = GetLocaleInfoEx(localeName.data(), LCType, nullptr, 0);
    if (infoLen > 0)
    {
        std::wstring infoBuf(static_cast<size_t>(infoLen) - 1, '\0');
        if (GetLocaleInfoEx(localeName.data(), LCType, infoBuf.data(), infoLen) > 0)
        {
            return infoBuf;
        }
    }
    return {};
}

} // namespace

auto Ime::InputMethodManager::Initialize(ITfThreadMgr *threadMgr, TfClientId clientId) -> HRESULT
{
    logger::debug("Initializing LangProfileUtil...");
    m_threadMgr = CComQIPtr<ITfThreadMgr>(threadMgr);
    m_clientId  = clientId;
    if (m_threadMgr == nullptr) return E_FAIL;

    if (CComQIPtr<ITfSource> const lpSource(m_threadMgr); lpSource != nullptr)
    {
        if (SUCCEEDED(lpSource->AdviseSink(IID_ITfInputProcessorProfileActivationSink, this, &m_dwCookie)))
        {
            if (SUCCEEDED(m_tfProfileMgr.CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER)))
            {
                // RefreshProfiles re-anchors the active-profile cache itself.
                RefreshProfiles();
                return S_OK;
            }
        }
    }
    return E_FAIL;
}

auto Ime::InputMethodManager::UnInitialize() -> void
{
    if (m_threadMgr != nullptr)
    {
        if (CComQIPtr<ITfSource> const lpSource(m_threadMgr); lpSource != nullptr)
        {
            lpSource->UnadviseSink(m_dwCookie);
        }
        m_tfProfileMgr.Release();
        m_threadMgr.Release();
    }
    m_langProfiles.clear();
}

auto Ime::InputMethodManager::RefreshProfiles() -> bool
{
    m_langProfiles.clear();

    // E_FAIL, not TRUE (== 1): if anything below throws before a real HRESULT
    // is stored, SUCCEEDED(1) would misreport a partially-read (empty) profile
    // list as a success.
    HRESULT hresult = E_FAIL;
    try
    {
        _tsetlocale(LC_ALL, _T(""));
        CComPtr<ITfInputProcessorProfiles> lpProfiles;
        hresult = lpProfiles.CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER);
        Tsf::throw_fail(hresult, "Failed create ITfInputProcessorProfiles");

        CComPtr<IEnumTfInputProcessorProfiles> lpEnum;
        hresult = m_tfProfileMgr->EnumProfiles(0, &lpEnum);
        Tsf::throw_fail(hresult, "Failed enum language profiles");

        TF_INPUTPROCESSORPROFILE profile = {};
        ULONG                    fetched = 0;
        while (lpEnum->Next(1, &profile, &fetched) == S_OK)
        {
            if ((profile.dwFlags & TF_IPP_FLAG_ENABLED) == 0) continue;
            // if (profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT) continue; should allow keyboard layout?
            if (profile.catid == GUID_NULL) continue; ///< "触控输入更正" profile has no catid.

            BOOL bEnabled = FALSE;
            // Skip profile that failed to load.
            if (FAILED(lpProfiles->IsEnabledLanguageProfile(profile.clsid, profile.langid, profile.guidProfile, &bEnabled)) || bEnabled == FALSE)
            {
                continue;
            }

            const auto localeName = GetLocaleName(profile.langid);

            auto localeDisplayName = GetLocaleInfo(localeName, LOCALE_SLOCALIZEDDISPLAYNAME);
            auto language          = GetLocaleInfo(localeName, LOCALE_SLOCALIZEDLANGUAGENAME);
            auto desc              = GetLangProfileDesc(lpProfiles, profile);
            if (desc.empty())
            {
                // A missing display name must not drop the profile: it would
                // vanish from the in-game switcher while still being the user's
                // active input method. Keep it under a technical name instead.
                logger::error(
                    "No display name for input method profile (clsid {}, langid {:#x}, guidProfile {}); keeping it with a fallback name",
                    WCharUtils::ToString(ToStringFromGUID2(profile.clsid)),
                    profile.langid,
                    WCharUtils::ToString(ToStringFromGUID2(profile.guidProfile))
                );
                desc = std::format(
                    "{} ({})", WCharUtils::ToString(localeName), WCharUtils::ToString(ToStringFromGUID2(profile.guidProfile)));
            }
            std::string localeDisplayNameStr = WCharUtils::ToString(localeDisplayName);
            if (localeDisplayNameStr.empty())
            {
                localeDisplayNameStr = WCharUtils::ToString(localeName);
            }
            // The trailing identity string is what [core] default_input_method
            // accepts — users copy it straight out of the log. Keyboard layouts
            // get keyboard:{KLID}; an unresolvable HKL has no persistable form.
            std::string identityText;
            if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
            {
                identityText = InputMethodIdentity::FormatFromTip(
                    WCharUtils::ToString(ToStringFromGUID2(profile.clsid)),
                    WCharUtils::ToString(ToStringFromGUID2(profile.guidProfile)),
                    profile.langid
                );
            }
            else
            {
                const auto klid = ResolveKlid(profile.hkl);
                identityText    = klid.empty() ? std::string{"<keyboard layout, KLID not resolvable>"} : InputMethodIdentity::FormatFromKeyboard(klid);
            }
            logger::info("Load installed ime: {} {} ({})", localeDisplayNameStr, desc, identityText);
            m_langProfiles.emplace_back(
                std::move(localeDisplayNameStr),
                std::move(desc),
                WCharUtils::ToString(language),
                profile.clsid,
                profile.guidProfile,
                profile.langid,
                profile.dwProfileType,
                profile.hkl
            );
        }
    }
    catch (const std::runtime_error &error)
    {
        logger::error("LoadIme failed: {}", error.what());
    }
    catch (...)
    {
        // Non-runtime_error exceptions (bad_alloc, COM E_OUTOFMEMORY wrappers)
        // must not escape: a partial list + the stale hresult would otherwise
        // look like a success to the caller.
        logger::error("LoadIme failed: unknown exception.");
        hresult = E_FAIL;
    }
    if (SUCCEEDED(hresult))
    {
        // The rebuilt list invalidates every cached index (installs/uninstalls
        // reorder or drop entries), so re-anchor the active profile before
        // IsConfiguredProfileActive or GetActiveLangProfile reads a stale one.
        // GetSelectableInputMethods(true) refreshes from the settings window,
        // so this path runs mid-session, not only at startup.
        UpdateActiveProfile();
    }
    return SUCCEEDED(hresult);
}

auto Ime::InputMethodManager::UpdateActiveProfile() noexcept -> bool
{
    m_activatedProfile = 0;
    TF_INPUTPROCESSORPROFILE profile;
    if (SUCCEEDED(m_tfProfileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile)))
    {
        m_activatedProfile = GetProfileCachedIndex(m_langProfiles, profile.guidProfile, profile.langid, profile.hkl);
        // Semantics: "any profile known to us is active". This flag gates text
        // forwarding (ImeWnd WM_CHAR) and char-event swallowing (ImeMenu) — do
        // NOT narrow it to INPUTPROCESSOR-only, users whose active input method
        // is a plain keyboard layout (e.g. English-only systems) would lose all
        // text input. Whether a real TIP (vs. a keyboard layout) is active is
        // checked at the display site via the profile type instead.
        Core::State::GetInstance().Set(State::INPUT_PROCESSOR_ACTIVATED, m_activatedProfile < m_langProfiles.size());
        return true;
    }

    logger::error("Load active language profile failed.");
    return false;
}

auto Ime::InputMethodManager::ActivateProfile(const GUID &guidProfile) -> HRESULT
{
    if (guidProfile == DEFAULT_LANG_PROFILE.guidProfile)
    {
        // GUID_NULL is not an activatable profile: keyboard layouts all share
        // it, and DEFAULT_LANG_PROFILE itself is a display-only stub (CLSID_NULL
        // / GUID_NULL / no HKL match nothing in TSF). Interpret "activate the
        // default" as "switch to the English keyboard".
        return ActivateKeyboardEng();
    }

    const auto index = GetProfileCachedIndex(m_langProfiles, guidProfile);
    if (index >= m_langProfiles.size())
    {
        return E_INVALIDARG;
    }

    return ActivateProfile(m_langProfiles[index]);
}

auto Ime::InputMethodManager::ActivateProfileByIndex(std::uint32_t index) -> HRESULT
{
    if (index >= m_langProfiles.size())
    {
        logger::warn("ActivateProfileByIndex({}) is out of range ({} profiles)", index, m_langProfiles.size());
        return E_INVALIDARG;
    }
    return ActivateProfile(m_langProfiles[index]);
}

auto Ime::InputMethodManager::ActivateProfile(const LangProfile &langProfile) -> HRESULT
{
    // Keyboard layout profiles (TF_PROFILETYPE_KEYBOARDLAYOUT) ALL share
    // guidProfile == GUID_NULL. The "already active" shortcut below would match
    // against whatever GetActiveLangProfile() returns — including DEFAULT_LANG_PROFILE
    // (also GUID_NULL) when the cached activated index is stale or out of range —
    // so ActivateProfile() would ALWAYS return S_OK here without ever calling the
    // TSF API, leaving the Chinese TIP active and eating keystrokes. Only real TIPs
    // (INPUTPROCESSOR) are eligible for the shortcut; keyboard layouts always go
    // through the TSF call.
    if (langProfile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
    {
        // SameTipVariant, not a bare GUID compare: a multi-language TIP is
        // enumerated once per language with an IDENTICAL guidProfile, so the
        // GUID-only check used to report the zh variant "already active" while
        // the caller had asked for the ja one — the TSF call was skipped and
        // typing silently kept the wrong IME (the settings pick never landed).
        // ToIdentity maps the stale-index stub (a KEYBOARDLAYOUT) to
        // Kind::Keyboard, so a stale cache degrades to a redundant, harmless
        // re-activation instead of a wrong skip.
        if (InputMethodIdentity::SameTipVariant(ToIdentity(langProfile), ToIdentity(GetActiveLangProfile())))
        {
            logger::debug("Profile '{}' already active, skip activation", langProfile.desc);
            return S_OK;
        }
    }
    // The sink fires synchronously inside this call. Without the pause every
    // switch this manager makes (the disable path's English keyboard, the
    // configured default, the fallback) would be recorded as a user pick and the
    // restore would then replay the mod's own choice instead of the user's.
    HRESULT hresult = E_FAIL;
    {
        ScopedSelfActivation const pause{*this};
        hresult = m_tfProfileMgr->ActivateProfile(
            langProfile.dwProfileType,
            langProfile.langid,
            langProfile.clsid,
            langProfile.guidProfile,
            langProfile.hkl,
            TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE
        );
    }
    if (FAILED(hresult))
    {
        logger::error("Active profile {} failed: {}", langProfile.desc, Tsf::ToErrorMessage(hresult));
    }
    else
    {
        logger::info("Active profile {} OK (type={}, hkl={:p})", langProfile.desc, langProfile.dwProfileType, static_cast<void *>(langProfile.hkl));
    }
    return hresult;
}

auto Ime::InputMethodManager::ActivateKeyboardProfile(const std::string &klid) -> HRESULT
{
    if (!InputMethodIdentity::Detail::IsKlidText(klid))
    {
        logger::warn("ActivateKeyboardProfile('{}') is not a KLID (8 hex digits)", klid);
        return E_INVALIDARG;
    }

    // Deliberately NO "already active, skip" short-circuit here (unlike the TIP
    // path): the TSF call can report S_OK without switching, so the Imm32
    // ActivateKeyboardLayout(KLF_SETFORPROCESS) below must run every time this is
    // called. A redundant switch is harmless; a skipped one leaves the user on
    // the wrong layout.
    //
    // LoadKeyboardLayout both validates the KLID and yields a handle that is
    // guaranteed live, unlike TF_INPUTPROCESSORPROFILE::hkl, which may be stale
    // by the time we get here.
    const HKL hkl = LoadKeyboardLayoutW(ToWideKlid(klid).c_str(), KLF_ACTIVATE);
    if (hkl == nullptr)
    {
        logger::warn("LoadKeyboardLayout('{}') failed: {}", klid, GetLastError());
        return E_FAIL;
    }

    // Prefer the enumerated entry (it carries the localized display name and the
    // real langid); fall back to a synthetic profile so a layout that is
    // installed but not enumerated for this process still activates.
    const LangProfile *enumerated = nullptr;
    for (const auto &langProfile : m_langProfiles)
    {
        if (langProfile.dwProfileType != TF_PROFILETYPE_KEYBOARDLAYOUT) continue;
        if (!InputMethodIdentity::Detail::SameText(ToIdentity(langProfile).klid, klid)) continue;
        enumerated = &langProfile;
        break;
    }
    LangProfile  synthetic;
    LangProfile *target = nullptr;
    if (enumerated != nullptr)
    {
        synthetic             = *enumerated;
        synthetic.hkl         = hkl;
        target                = &synthetic;
    }
    else
    {
        synthetic = LangProfile{"", klid, "", CLSID_NULL, GUID_NULL, static_cast<LANGID>(std::stoul(klid.substr(4), nullptr, 16)), TF_PROFILETYPE_KEYBOARDLAYOUT, hkl};
        target    = &synthetic;
    }

    // One pause around BOTH steps: ActivateProfile's TSF call and the Imm32
    // ActivateKeyboardLayout below each fire the activation sink on this thread,
    // and both are this manager's own doing — the user did not pick this layout
    // just now (it is either the disable path's English switch or a configured
    // default). ActivateProfile nests its own pause; the counter makes that safe.
    ScopedSelfActivation const pause{*this};
    const HRESULT              hresult = ActivateProfile(*target);
    // ActivateProfile returning S_OK does NOT mean the system input method
    // actually switched: for KEYBOARDLAYOUT profiles TSF often reports success
    // while leaving the process input method untouched (observed with WeChat IME
    // / Microsoft Pinyin). Force the PROCESS keyboard layout at the Imm32/Win32
    // level — TSF TIPs follow the active keyboard layout and deactivate with it,
    // so this is the authoritative switch. Always do it, and verify.
    if (ActivateKeyboardLayout(hkl, KLF_SETFORPROCESS) == nullptr)
    {
        logger::warn("ActivateKeyboardLayout(KLF_SETFORPROCESS) failed: {}", GetLastError());
    }
    else
    {
        const auto currentLayout = GetKeyboardLayout(0); // current thread layout
        logger::info(
            "ActivateKeyboardLayout(KLF_SETFORPROCESS) OK, current={:p}, wanted={:p}{}",
            static_cast<void *>(currentLayout),
            static_cast<void *>(hkl),
            currentLayout != hkl ? " (MISMATCH!)" : ""
        );
    }
    return hresult;
}

auto Ime::InputMethodManager::ActivateKeyboardEng() -> HRESULT
{
    // "Switch back to English" means the en-US layout throughout this mod, and
    // ActivateKeyboardProfile already does load + TSF activation + Imm32
    // KLF_SETFORPROCESS + verification in one place. Reuse it instead of keeping
    // a second copy of that sequence (the two used to drift apart).
    const HRESULT hresult = ActivateKeyboardProfile("00000409");
    if (SUCCEEDED(hresult)) return hresult;

    // LoadKeyboardLayout("00000409") failed — the layout is not installed here.
    // Fall back to an English entry from the enumeration instead of giving up:
    // the language bar's English TIP/keyboard is still better than leaving a
    // Chinese IME eating the game's WASD.
    static constexpr auto LANGID_ENGLISH_PRIMARY = PRIMARYLANGID(LANGID_ENG);
    logger::warn("Cannot load the en-US layout; falling back to an enumerated English profile");
    for (const auto &langProfile : m_langProfiles)
    {
        if (PRIMARYLANGID(langProfile.langid) == LANGID_ENGLISH_PRIMARY)
        {
            return ActivateProfile(langProfile);
        }
    }
    logger::error("No English keyboard profile found in langProfiles and LoadKeyboardLayout failed; cannot switch to English");
    return E_FAIL;
}

auto Ime::InputMethodManager::ActivatePreferredImeProfile() -> HRESULT
{
    // The user's first non-English TIP (input processor) from the enumeration.
    // Only real TIPs qualify: a plain keyboard layout cannot compose CJK text,
    // which is the entire point of this fallback.
    for (const auto &langProfile : m_langProfiles)
    {
        if (langProfile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && PRIMARYLANGID(langProfile.langid) != PRIMARYLANGID(LANGID_ENG))
        {
            logger::info("No input method remembered from the previous session; activating the user's IME '{}'", langProfile.desc);
            return ActivateProfile(langProfile);
        }
    }
    logger::debug("No non-English TIP found in the profile list; keeping the current input method");
    return E_FAIL;
}

auto Ime::InputMethodManager::ActivateIdentity(const InputMethodIdentity::Identity &identity) -> HRESULT
{
    using Kind = InputMethodIdentity::Identity::Kind;
    switch (identity.kind)
    {
        case Kind::Keyboard:
            return ActivateKeyboardProfile(identity.klid);
        case Kind::Tip:
        {
            for (const auto &langProfile : m_langProfiles)
            {
                if (langProfile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR) continue;
                if (!InputMethodIdentity::Matches(identity, ToIdentity(langProfile))) continue;
                return ActivateProfile(langProfile);
            }
            return E_INVALIDARG;
        }
        case Kind::Desc:
        case Kind::Unset:
        case Kind::Unknown:
            break;
    }
    return E_INVALIDARG;
}

auto Ime::InputMethodManager::IsIdentityActive(const InputMethodIdentity::Identity &identity) const -> bool
{
    if (m_activatedProfile >= m_langProfiles.size()) return false;
    return InputMethodIdentity::Matches(identity, ToIdentity(m_langProfiles[m_activatedProfile]));
}

auto Ime::InputMethodManager::HasRememberedUserProfile() const -> bool
{
    using Kind = InputMethodIdentity::Identity::Kind;
    switch (m_lastUserProfile.kind)
    {
        case Kind::Tip:
            return true;
        case Kind::Keyboard:
            return !m_lastUserProfile.klid.empty();
        default:
            return false;
    }
}

auto Ime::InputMethodManager::RestoreRememberedUserProfile() -> HRESULT
{
    if (!HasRememberedUserProfile()) return S_FALSE;
    // Copy: a failed restore clears the memory below.
    const auto identity = m_lastUserProfile;
    const auto hr       = ActivateIdentity(identity);
    if (SUCCEEDED(hr))
    {
        logger::info("Restored the input method the user last picked: '{}'", identity.desc);
        return hr;
    }
    // Uninstalled since it was picked (or a stale langid variant): drop the
    // memory so the caller's fallback decides instead of retrying forever.
    m_lastUserProfile = {};
    logger::warn(
        "The input method the user last picked ('{}', {}) is not available any more; forgetting it",
        identity.desc.empty() ? std::string{"?"} : identity.desc,
        identity.kind == InputMethodIdentity::Identity::Kind::Keyboard ? InputMethodIdentity::FormatFromKeyboard(identity.klid)
                                                                       : InputMethodIdentity::FormatFromTip(identity.clsid, identity.profile, identity.langid)
    );
    return hr;
}

auto Ime::InputMethodManager::BeginSelfActivation() -> void
{
    ++m_suppressUserTracking;
}

auto Ime::InputMethodManager::EndSelfActivation() -> void
{
    // Guard the decrement: a stray pair would otherwise leave the counter
    // negative and silence user tracking for the rest of the session.
    if (m_suppressUserTracking > 0) --m_suppressUserTracking;
}

auto Ime::InputMethodManager::GetSelectableChoices() const -> std::vector<InputMethodChoice>
{
    using Kind = InputMethodIdentity::Identity::Kind;

    std::vector<InputMethodChoice> choices;
    for (const auto &langProfile : m_langProfiles)
    {
        // Both kinds are offered: a TIP composes text, and a keyboard layout is a
        // legitimate "stay in English this session" pick. A layout whose KLID did
        // not resolve has no storable identity, so it cannot be chosen (its row
        // would store keyboard: and never match again).
        const auto      identity = ToIdentity(langProfile);
        InputMethodChoice choice;
        switch (identity.kind)
        {
            case Kind::Tip:
                choice.identity = InputMethodIdentity::FormatFromTip(identity.clsid, identity.profile, identity.langid);
                break;
            case Kind::Keyboard:
                if (identity.klid.empty()) continue;
                choice.identity = InputMethodIdentity::FormatFromKeyboard(identity.klid);
                break;
            default:
                continue;
        }
        choice.name = langProfile.desc;

        // The same profile can be reported twice (one entry per registered
        // language for a multi-language TIP once the langid suffix differs is
        // fine, but an exact duplicate would show as two identical rows).
        const auto duplicate = std::ranges::any_of(choices, [&choice](const InputMethodChoice &existing) -> bool {
            return InputMethodIdentity::Detail::SameText(existing.identity, choice.identity);
        });
        if (duplicate) continue;
        choices.push_back(std::move(choice));
    }

    std::ranges::stable_sort(choices, [](const InputMethodChoice &lhs, const InputMethodChoice &rhs) -> bool { return lhs.name < rhs.name; });
    return choices;
}

auto Ime::InputMethodManager::ShouldApplyConfiguredProfile() const -> bool
{
    return m_reapplyOnEnable || m_pendingApply;
}

auto Ime::InputMethodManager::SetConfiguredProfile(std::string configuredProfile, const bool reapplyOnEnable) -> void
{
    // A changed pick owes one apply even when the policy is "game start only":
    // the user just chose it and expects it to stick right away.
    const bool changed  = configuredProfile != m_configuredProfile;
    m_pendingApply       = m_pendingApply || changed;
    // A new value deserves a fresh "not available" toast (see
    // ActivateConfiguredProfile): the enable path retries on every forced sync.
    if (changed) m_unavailableNotified = false;
    m_configuredProfile  = std::move(configuredProfile);
    m_reapplyOnEnable    = reapplyOnEnable;
    m_configuredIdentity = InputMethodIdentity::Parse(m_configuredProfile);

    using Kind = InputMethodIdentity::Identity::Kind;
    switch (m_configuredIdentity.kind)
    {
        case Kind::Unset:
            logger::info("Default input method: not configured, the system input method is left alone");
            break;
        case Kind::Unknown:
            logger::warn(
                "Default input method '{}' cannot be parsed; ignoring it (expected 'tip:<clsid>/<guid>[/<langid>]' or a display name)",
                m_configuredProfile
            );
            break;
        case Kind::Keyboard:
            // A layout is not a text service — it is a plain "stay in English"
            // pick — but it is selected and remembered exactly like a TIP.
            logger::info(
                "Default input method set to '{}' ({})",
                m_configuredProfile,
                m_reapplyOnEnable ? "applied on every IME enable" : "applied on game start only"
            );
            break;
        case Kind::Tip:
        case Kind::Desc:
            logger::info(
                "Default input method set to '{}' ({})",
                m_configuredProfile,
                m_reapplyOnEnable ? "applied on every IME enable" : "applied on game start only"
            );
            break;
    }
}

auto Ime::InputMethodManager::HasConfiguredProfile() const -> bool
{
    using Kind = InputMethodIdentity::Identity::Kind;
    switch (m_configuredIdentity.kind)
    {
        case Kind::Tip:
        case Kind::Desc:
            return true;
        case Kind::Keyboard:
            // Without a KLID the identity cannot be matched again (see
            // InputMethodIdentity::Matches), so treat it as not configured.
            return !m_configuredIdentity.klid.empty();
        default:
            return false;
    }
}

auto Ime::InputMethodManager::IsConfiguredProfileActive() const -> bool
{
    using Kind = InputMethodIdentity::Identity::Kind;
    if (m_configuredIdentity.kind == Kind::Unset || m_configuredIdentity.kind == Kind::Unknown) return false;
    return IsIdentityActive(m_configuredIdentity);
}

auto Ime::InputMethodManager::ActivateConfiguredProfile() -> HRESULT
{
    using Kind = InputMethodIdentity::Identity::Kind;
    if (m_configuredIdentity.kind == Kind::Unset) return S_FALSE;
    if (!HasConfiguredProfile())
    {
        logger::warn("Default input method '{}' is not usable; nothing to activate", m_configuredProfile);
        return E_INVALIDARG;
    }

    // Both kinds are matchable: a keyboard layout is a legitimate default (e.g.
    // "always come up in English"). Matches() only hits a candidate of the same
    // kind, so a tip: identity can never select a layout or vice versa.
    const LangProfile *target = nullptr;
    for (const auto &langProfile : m_langProfiles)
    {
        if (!InputMethodIdentity::Matches(m_configuredIdentity, ToIdentity(langProfile))) continue;
        target = &langProfile;
        break;
    }

    // A hand-written display name is matched loosely as a second pass, so
    // default_input_method = "拼音" works without anyone having to dig an
    // identity string out of the log.
    if (target == nullptr && m_configuredIdentity.kind == Kind::Desc)
    {
        const auto wanted = InputMethodIdentity::Detail::Trim(m_configuredIdentity.desc);
        std::size_t matches = 0;
        std::string matchedNames;
        for (const auto &langProfile : m_langProfiles)
        {
            if (!ContainsNoCase(langProfile.desc, wanted)) continue;
            if (target == nullptr) target = &langProfile;
            ++matches;
            if (matches <= 8)
            {
                if (!matchedNames.empty()) matchedNames += " | ";
                matchedNames += langProfile.desc;
            }
        }
        if (target != nullptr)
        {
            if (matches > 1)
            {
                logger::warn(
                    "Default input method '{}' matches {} input methods ({}); using the first one — an exact 'tip:...' or 'keyboard:...' identity is unambiguous",
                    m_configuredProfile,
                    matches,
                    matchedNames
                );
            }
            else
            {
                logger::info("Default input method '{}' matched the display name '{}'", m_configuredProfile, target->desc);
            }
        }
    }

    if (target != nullptr)
    {
        logger::info("Activating the configured default input method '{}'", target->desc);
        // Route through ActivateIdentity so a keyboard layout also gets the
        // Imm32 KLF_SETFORPROCESS switch (ActivateProfile alone can report S_OK
        // while the process layout stays put). ActivateProfile()/the identity
        // path both skip the TSF call when the profile is already active, so
        // callers may call this unconditionally.
        const auto hr = ActivateIdentity(ToIdentity(*target));
        if (SUCCEEDED(hr))
        {
            m_pendingApply = false; // the change this flag covered just landed
        }
        return hr;
    }

    // A keyboard layout that is installed but not enumerated for this process
    // still has a perfectly good KLID: LoadKeyboardLayout validates it and
    // BuildKeyboardProfile synthesizes the profile, so a hand-written
    // 'keyboard:00000409' works even when the enumeration skipped it.
    if (m_configuredIdentity.kind == Kind::Keyboard && !m_configuredIdentity.klid.empty())
    {
        logger::info("Activating the configured default keyboard layout '{}' (not enumerated for this process)", m_configuredIdentity.klid);
        const auto hr = ActivateKeyboardProfile(m_configuredIdentity.klid);
        if (SUCCEEDED(hr))
        {
            m_pendingApply = false;
            return hr;
        }
        // Fall through: the warning below lists what IS available.
    }

    // Never fall back here: the caller decides (startup keeps the current
    // profile, the enable path falls back to ActivatePreferredImeProfile()).
    // Log every candidate so a wrong identity string is diagnosable in one run.
    std::string candidates;
    for (const auto &langProfile : m_langProfiles)
    {
        const auto identity = ToIdentity(langProfile);
        std::string identityText;
        if (identity.kind == Kind::Tip)
        {
            identityText = InputMethodIdentity::FormatFromTip(identity.clsid, identity.profile, identity.langid);
        }
        else if (identity.kind == Kind::Keyboard && !identity.klid.empty())
        {
            identityText = InputMethodIdentity::FormatFromKeyboard(identity.klid);
        }
        else
        {
            continue; // unusable identity: nothing the user could have typed
        }
        if (!candidates.empty()) candidates += " | ";
        candidates += langProfile.desc;
        candidates += " (";
        candidates += identityText;
        candidates += ")";
    }
    logger::warn(
        "Configured default input method '{}' is not installed/enabled for this process; keeping the current input method. Candidates: {}",
        m_configuredProfile,
        candidates.empty() ? std::string{"<none>"} : candidates
    );
    // Surface it in the UI once per configured value: with the reapply policy
    // on, the enable path retries this on every forced sync (window focus
    // churn), and a toast each time would spam the player mid-game.
    if (!m_unavailableNotified)
    {
        m_unavailableNotified = true;
        ErrorNotifier::GetInstance().Warning("The configured default input method is not available; keeping the current one.");
    }
    return E_INVALIDARG;
}

auto Ime::InputMethodManager::GetActiveLangProfile() -> const LangProfile &
{
    if (m_activatedProfile >= m_langProfiles.size())
    {
        return DEFAULT_LANG_PROFILE;
    }
    return m_langProfiles[m_activatedProfile];
}

auto Ime::InputMethodManager::QueryInterface(const IID &riid, void **ppvObject) -> HRESULT
{
    *ppvObject = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfInputProcessorProfileActivationSink))
    {
        *ppvObject = static_cast<ITfInputProcessorProfileActivationSink *>(this);
    }

    if (*ppvObject != nullptr)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

auto Ime::InputMethodManager::AddRef() -> ULONG
{
    return ++m_refCount;
}

auto Ime::InputMethodManager::Release() -> ULONG
{
    --m_refCount;
    if (m_refCount == 0)
    {
        delete this;
        return 0;
    }
    return m_refCount;
}

auto Ime::InputMethodManager::OnActivated(
    DWORD dwProfileType, [[maybe_unused]] LANGID langid, [[maybe_unused]] const IID &clsid, [[maybe_unused]] const GUID &catid,
    const GUID &guidProfile, HKL hkl, DWORD dwFlags
) -> HRESULT
{
    if ((dwFlags & TF_IPSINK_FLAG_ACTIVE) != 0)
    {
        auto &state = State::GetInstance();
        // Do NOT wipe the conversion mode here unconditionally: IMEs that
        // publish no conversion compartment (WeChat IME — verified: thread,
        // context and global compartments all stay VT_EMPTY for whole
        // sessions) leave the refresher below with nothing to read, so the
        // wipe was the last writer and the bar fell back to 英 on EVERY
        // disable/enable cycle even though the restored TIP was still in
        // Chinese mode. Keep the last observed mode and let the real sources
        // overwrite it: the refresher when a compartment carries a value,
        // behavioral inference within one keystroke otherwise (composition
        // start asserts NATIVE, a raw letter retracts it).
        //
        // The one deliberate wipe: a manual switch to a Chinese keyboard
        // LAYOUT (中文-美式键盘, a KEYBOARD-type profile with a Chinese
        // langid). No TIP mode exists there and the bar renders the 中/英
        // label for Chinese langids, so a remembered NATIVE would mislabel
        // it. The disable path's 美式键盘 (langid 0x409) renders no label
        // and must NOT wipe — the memory it would clear is exactly what the
        // next enable's restore needs.
        if (dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR && PRIMARYLANGID(langid) == LANG_CHINESE)
        {
            state.ClearConversionMode();
        }

        UpdateConversionAndKeyboard(state, dwProfileType);

        // First-entry prior: with no observation this session, a Chinese TIP
        // is overwhelmingly in Chinese mode — show 中 instead of the
        // ALPHANUMERIC startup default. A wrong guess self-corrects on the
        // first keystroke via behavioral inference; the 英 default was wrong
        // for every Chinese user on every first entry. Any real observation
        // (compartment write, Shift toggle, inference) marks the mode as
        // observed through the State mutators, so this seed only ever fires
        // for a genuinely unknown mode. Normal first entries never reach it:
        // ImeApp seeds the mode from the persisted last-native-conversion
        // cache at startup, which marks it observed. What remains is the
        // manual 中文-美式键盘 bounce (its wipe above resets the observed
        // flag) — 中 is still the best guess for the TIP the user returns to.
        if (dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && PRIMARYLANGID(langid) == LANG_CHINESE && !state.HasObservedConversionMode())
        {
            state.AddConversionModeFlag(State::ConversionMode::Flags::NATIVE);
            logger::info("No conversion-mode observation yet; seeding 中 for the Chinese TIP");
        }

        m_activatedProfile = GetProfileCachedIndex(m_langProfiles, guidProfile, langid, hkl);
        // Same semantics as UpdateActiveProfile: any known profile counts.
        // See the comment there — do not narrow to INPUTPROCESSOR-only.
        Core::State::GetInstance().Set(State::INPUT_PROCESSOR_ACTIVATED, m_activatedProfile < m_langProfiles.size());
        // Remember whatever the user actually landed on — TIP or plain keyboard
        // layout. This is the memory the next IME enable restores, so it must
        // cover both kinds: "I switched to 美式键盘 and closed the console" has
        // to survive the round trip exactly like a TIP pick does.
        //
        // Activations triggered by this class itself (the disable path's
        // forced English layout, our own configured-profile apply) are
        // suppressed: those are not user choices, and recording them would
        // erase the very pick being restored. See ScopedSelfActivation.
        if (m_suppressUserTracking == 0 && m_activatedProfile < m_langProfiles.size() &&
            (dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR || dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT))
        {
            m_lastUserProfile = ToIdentity(m_langProfiles[m_activatedProfile]);
            if (HasRememberedUserProfile())
            {
                logger::info("Remembered the user's input method: '{}'", m_langProfiles[m_activatedProfile].desc);
            }
            else
            {
                m_lastUserProfile = {}; // e.g. an unresolvable KLID: nothing to restore
            }
        }
        if (m_activatedProfile < m_langProfiles.size())
        {
            logger::info(
                "Input method activated: '{}' (type={}, hkl={:p}, flags={:#x})",
                m_langProfiles[m_activatedProfile].desc,
                dwProfileType,
                static_cast<void *>(hkl),
                dwFlags
            );
        }
        state.ClearComposing();
    }
    return S_OK;
}

auto Ime::InputMethodManager::UpdateConversionAndKeyboard(State &state, DWORD dwProfileType) -> void
{
    // Conversion mode is deliberately NOT read from the thread-level
    // INPUTMODE_CONVERSION compartment here anymore:
    //  - A keyboard-layout activation (the disable path's 美式键盘) carries no
    //    IME mode at all; combined with the ClearConversionMode() in OnActivated,
    //    every disable/enable cycle re-seeded State to ALPHANUMERIC and the
    //    language bar fell back to "英" even though the restored TIP was still
    //    in Chinese mode.
    //  - For a TIP the live value lives on the FOCUSED CONTEXT's compartment
    //    (WeChat IME only maintains that one — it is what the OS mode indicator
    //    reads). The text service owns that watcher, so delegate the re-read to
    //    it: context value first, thread default as fallback.
    if (dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && m_conversionModeRefresher != nullptr)
    {
        m_conversionModeRefresher();
    }

    if (const CComQIPtr<ITfCompartmentMgr> compartmentMgr(m_threadMgr); compartmentMgr != nullptr)
    {
        if (!state.ImeDisabled())
        {
            CComPtr<ITfCompartment> keyboardOpenCloseCompartment;
            if (SUCCEEDED(compartmentMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &keyboardOpenCloseCompartment)))
            {
                // VT_I4 with lVal (LONG, 4 bytes). The old code wrote boolVal
                // (VARIANT_BOOL, 2 bytes) into an uninitialized VARIANT — the
                // high half of the lVal the consumers read was stack garbage.
                CComVariant var; // VariantInit: fully zeroed
                var.vt   = VT_I4;
                var.lVal = TRUE;
                if (FAILED(keyboardOpenCloseCompartment->SetValue(m_clientId, &var)))
                {
                    ErrorNotifier::GetInstance().Warning("Can't open keyboard, IME may can't work.");
                }
            }
        }
    }
}
