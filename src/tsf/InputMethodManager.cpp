#include "tsf/InputMethodManager.h"

#include "WCharUtils.h"
#include "core/State.h"
#include "imguiex/ErrorNotifier.h"
#include "log.h"

#include <Shlwapi.h>
#include <future>
#include <msctf.h>
#include <stdexcept>

#pragma comment(lib, "Shlwapi.lib")

namespace
{
auto GetProfileCachedIndex(const std::vector<Ime::LangProfile> &langProfiles, const GUID &guidProfile, HKL hkl = nullptr) -> std::uint32_t
{
    if (guidProfile != GUID_NULL)
    {
        // Real TIPs (input processors) carry unique profile GUIDs.
        const auto it = std::ranges::find_if(langProfiles, [&](const auto &p) -> bool {
            return p.guidProfile == guidProfile;
        });
        if (it != langProfiles.end())
        {
            return static_cast<std::uint32_t>(std::ranges::distance(langProfiles.begin(), it));
        }
        return UINT32_MAX;
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
                RefreshProfiles();
                UpdateActiveProfile();
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
            logger::info("Load installed ime: {} {}", localeDisplayNameStr, desc);
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
    return SUCCEEDED(hresult);
}

auto Ime::InputMethodManager::UpdateActiveProfile() noexcept -> bool
{
    m_activatedProfile = 0;
    TF_INPUTPROCESSORPROFILE profile;
    if (SUCCEEDED(m_tfProfileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile)))
    {
        m_activatedProfile = GetProfileCachedIndex(m_langProfiles, profile.guidProfile, profile.hkl);
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

auto Ime::InputMethodManager::ActivateProfile(const LangProfile &langProfile) -> HRESULT
{
    // Keyboard layout profiles (TF_PROFILETYPE_KEYBOARDLAYOUT) ALL share
    // guidProfile == GUID_NULL. The "already active" shortcut below would match
    // against whatever GetActiveLangProfile() returns — including DEFAULT_LANG_PROFILE
    // (also GUID_NULL) when the cached activated index is stale or out of range —
    // so ActivateProfile() would ALWAYS return S_OK here without ever calling the
    // TSF API, leaving the Chinese TIP active and eating keystrokes. Only real TIPs
    // (INPUTPROCESSOR) carry distinguishable GUIDs; apply the shortcut to them alone.
    if (langProfile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
    {
        const auto &activeLangProfile = GetActiveLangProfile();
        if (IsEqualGUID(langProfile.guidProfile, activeLangProfile.guidProfile) == TRUE)
        {
            logger::debug("Profile '{}' already active, skip activation", langProfile.desc);
            return S_OK;
        }
    }
    const HRESULT hresult = m_tfProfileMgr->ActivateProfile(
        langProfile.dwProfileType,
        langProfile.langid,
        langProfile.clsid,
        langProfile.guidProfile,
        langProfile.hkl,
        TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE
    );
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

auto Ime::InputMethodManager::ActivateKeyboardEng() -> HRESULT
{
    // Find an English keyboard profile in the loaded profiles list and activate it.
    // DEFAULT_LANG_PROFILE has CLSID_NULL/GUID_NULL — stub values that don't match
    // any actual TSF profile, so it is never a valid activation target; use the
    // real profile from the enumeration instead.
    // PRIMARYLANGID matching covers all English variants (en-US 0x409, en-GB 0x809, ...).
    static constexpr auto LANGID_ENGLISH_PRIMARY = PRIMARYLANGID(LANGID_ENG);

    // Load the English keyboard layout to get a validated HKL handle.
    // The HKL from the TSF enumeration (TF_INPUTPROCESSORPROFILE::hkl) may be stale
    // or invalid by the time ActivateProfile is called. LoadKeyboardLayout returns
    // a fresh, valid HKL.
    const HKL hklEnglish = LoadKeyboardLayout(L"00000409", KLF_ACTIVATE);
    if (hklEnglish == nullptr)
    {
        logger::warn("LoadKeyboardLayout failed for English keyboard, falling back to enumeration HKL");
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

    for (const auto &langProfile : m_langProfiles)
    {
        if (PRIMARYLANGID(langProfile.langid) == LANGID_ENGLISH_PRIMARY)
        {
            // Use the fresh LoadKeyboardLayout HKL, which should be a valid handle
            LangProfile englishProfile = langProfile;
            englishProfile.hkl         = hklEnglish;
            const HRESULT hresult      = ActivateProfile(englishProfile);
            // ActivateProfile returning S_OK does NOT mean the system input method
            // actually switched: for KEYBOARDLAYOUT profiles TSF often reports
            // success while leaving the current process input method untouched
            // (observed with WeChat IME / Microsoft Pinyin). Force the PROCESS
            // keyboard layout at the Imm32/Win32 level — TSF TIPs follow the active
            // keyboard layout and deactivate with it, so this is the authoritative
            // switch. Always do it (not just on FAILED) and verify the result.
            if (ActivateKeyboardLayout(hklEnglish, KLF_SETFORPROCESS) == nullptr)
            {
                logger::warn("ActivateKeyboardLayout(KLF_SETFORPROCESS) failed: {}", GetLastError());
            }
            else
            {
                const auto currentLayout = GetKeyboardLayout(0); // current thread layout
                logger::info(
                    "ActivateKeyboardLayout(KLF_SETFORPROCESS) OK, current={:p}, english={:p}{}",
                    static_cast<void *>(currentLayout),
                    static_cast<void *>(hklEnglish),
                    currentLayout != hklEnglish ? " (MISMATCH!)" : ""
                );
            }
            return hresult;
        }
    }

    // Fallback: use LoadKeyboardLayout HKL with a synthetic profile.
    logger::warn("No English keyboard profile found in langProfiles, activating via LoadKeyboardLayout HKL");
    return ActivateProfile(LangProfile{"English", "ENG", "English", CLSID_NULL, GUID_NULL, LANGID_ENG, TF_PROFILETYPE_KEYBOARDLAYOUT, hklEnglish});
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

        m_activatedProfile = GetProfileCachedIndex(m_langProfiles, guidProfile, hkl);
        // Same semantics as UpdateActiveProfile: any known profile counts.
        // See the comment there — do not narrow to INPUTPROCESSOR-only.
        Core::State::GetInstance().Set(State::INPUT_PROCESSOR_ACTIVATED, m_activatedProfile < m_langProfiles.size());
        // Track the last REAL input processor (WeChat/Microsoft Pinyin etc.) that
        // activated. Keyboard-layout activations must not overwrite this — the
        // disable path switches to the English keyboard, which would otherwise
        // erase the memory of which IME the user was typing with.
        if (dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && m_activatedProfile < m_langProfiles.size())
        {
            m_lastTipProfileGuid = guidProfile;
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
