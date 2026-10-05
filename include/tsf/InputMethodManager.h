#ifndef TSF_LANGPROFILEUTIL_H
#define TSF_LANGPROFILEUTIL_H

#pragma once

#include "LangProfile.h"
#include "TsfSupport.h"
#include "core/State.h"

#include <functional>
#include <msctf.h>
#include <windows.h>

namespace Ime
{
class InputMethodManager : public ITfInputProcessorProfileActivationSink
{
    using State = Core::State;

    static constexpr auto DEFAULT_PROFILE_INDEX = 0;

public:
    InputMethodManager() = default;

    virtual ~InputMethodManager() { UnInitialize(); }

    InputMethodManager(const InputMethodManager &other)                         = delete;
    InputMethodManager(InputMethodManager &&other) noexcept                     = delete;
    auto operator=(const InputMethodManager &other) -> InputMethodManager &     = delete;
    auto operator=(InputMethodManager &&other) noexcept -> InputMethodManager & = delete;

    auto Initialize(ITfThreadMgr *threadMgr, TfClientId clientId) -> HRESULT;
    auto UnInitialize() -> void;

    auto RefreshProfiles() -> bool;
    auto UpdateActiveProfile() noexcept -> bool;
    auto ActivateProfile(const GUID &guidProfile) -> HRESULT;
    auto ActivateKeyboardEng() -> HRESULT;
    /// Activate the user's first non-English TIP (input processor) from the
    /// enumerated profile list. Fallback for re-enabling the IME when nothing
    /// was remembered (m_lastActiveProfile == GUID_NULL): at that point the
    /// current TIP is usually the English keyboard left over from an earlier
    /// disable, and silently keeping it means typing produces English only.
    auto ActivatePreferredImeProfile() -> HRESULT;

private:
    auto ActivateProfile(const LangProfile &langProfile) -> HRESULT;

public:
    auto GetActiveLangProfile() -> const LangProfile &;

    /// GUID of the last real TIP (input processor, e.g. WeChat/Microsoft Pinyin)
    /// that activated in this process. Unlike the active-profile cache it is NOT
    /// overwritten when a plain keyboard layout activates, so it always answers
    /// "which IME did the user last use" — used to restore the TIP on re-enable.
    [[nodiscard]] auto GetLastTipProfileGuid() const -> const GUID & { return m_lastTipProfileGuid; }

    [[nodiscard]] auto GetLangProfiles() const -> const std::vector<LangProfile> & { return m_langProfiles; }

    /// Injected by ImeWnd: delegates the conversion-mode re-read to the text
    /// service, which owns the focused-context compartment watcher (the live
    /// source) and the thread-level fallback. The activation sink itself must
    /// NOT read the thread compartment — see UpdateConversionAndKeyboard.
    auto SetConversionModeRefresher(std::function<void()> refresher) -> void { m_conversionModeRefresher = std::move(refresher); }

    auto QueryInterface(const IID &riid, void **ppvObject) -> HRESULT override;
    auto AddRef() -> ULONG override;
    auto Release() -> ULONG override;
    auto OnActivated(DWORD dwProfileType, LANGID langid, const IID &clsid, const GUID &catid, const GUID &guidProfile, HKL hkl, DWORD dwFlags)
        -> HRESULT override;

private:
    auto UpdateConversionAndKeyboard(State &state, DWORD dwProfileType) -> void;

    std::vector<LangProfile>             m_langProfiles;
    CComPtr<ITfInputProcessorProfileMgr> m_tfProfileMgr = nullptr;
    CComPtr<ITfThreadMgr>                m_threadMgr    = nullptr;
    TfClientId                           m_clientId;
    DWORD                                m_refCount{};
    DWORD                                m_dwCookie{};
    uint32_t                             m_activatedProfile = 0;
    GUID                                 m_lastTipProfileGuid{GUID_NULL};
    std::function<void()>                m_conversionModeRefresher = nullptr;
};
} // namespace Ime

#endif
