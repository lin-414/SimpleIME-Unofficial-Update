#ifndef TSF_LANGPROFILEUTIL_H
#define TSF_LANGPROFILEUTIL_H

#pragma once

#include "LangProfile.h"
#include "TsfSupport.h"
#include "core/State.h"
#include "ime/InputMethodChoice.h"
#include "utils/InputMethodIdentity.h"

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
    /// Activate by position in GetLangProfiles().
    /// Keyboard layouts all share guidProfile == GUID_NULL, so a GUID cannot
    /// name one of them: the language bar must pass an index to be able to pick
    /// the second English layout, or the second CJK IME of the same GUID family.
    auto ActivateProfileByIndex(std::uint32_t index) -> HRESULT;
    auto ActivateKeyboardEng() -> HRESULT;
    /// Activate the user's first non-English TIP (input processor) from the
    /// enumerated profile list. Fallback for re-enabling the IME when nothing
    /// was remembered (m_lastActiveProfile == GUID_NULL): at that point the
    /// current TIP is usually the English keyboard left over from an earlier
    /// disable, and silently keeping it means typing produces English only.
    auto ActivatePreferredImeProfile() -> HRESULT;

    /// The user's configured default input method ([core] default_input_method,
    /// an identity string parsed by utils/InputMethodIdentity.h). Pass a copy:
    /// IME thread only, never shared with the render thread that owns Settings.
    /// An empty value keeps the old behavior (never touch the input method).
    /// @param reapplyOnEnable also re-apply it on every IME enable, not only on
    ///        game start (Settings::reapplyDefaultInputMethodOnEnable).
    auto SetConfiguredProfile(std::string configuredProfile, bool reapplyOnEnable = true) -> void;
    /// Whether the caller (ImeManager::ApplyConfiguredInputMethod) may select the
    /// configured profile right now. False = apply once per game start, so a
    /// profile picked from the language bar afterwards sticks — except right
    /// after the user changed the pick in the settings window, where one apply is
    /// still owed so the change takes effect without a restart.
    [[nodiscard]] auto ShouldApplyConfiguredProfile() const -> bool;
    /// True when a usable identity is configured (parsable, and not the
    /// not-yet-supported keyboard-layout form).
    [[nodiscard]] auto HasConfiguredProfile() const -> bool;
    /// True when the profile reported active for this thread already is the
    /// configured one — callers use it to skip a redundant activation.
    [[nodiscard]] auto IsConfiguredProfileActive() const -> bool;
    /// Activate the configured input method. Returns E_INVALIDARG (and logs the
    /// raw value) when it is not installed/enabled; the caller owns the fallback
    /// decision — this never falls back on its own.
    auto ActivateConfiguredProfile() -> HRESULT;

    /// Every selectable input method (TSF input processor) of this process,
    /// sorted by display name — the source of the settings-window picker. Plain
    /// keyboard layouts are left out: their KLID cannot be derived from an HKL
    /// yet, so they are not selectable (P2). Reads the cached enumeration; call
    /// @c RefreshProfiles first when the caller wants a fresh list.
    [[nodiscard]] auto GetSelectableChoices() const -> std::vector<InputMethodChoice>;

private:
    auto ActivateProfile(const LangProfile &langProfile) -> HRESULT;

public:
    auto GetActiveLangProfile() -> const LangProfile &;
    /// Index of the active profile inside GetLangProfiles(), or UINT32_MAX when
    /// the cached index is stale (GetActiveLangProfile() then returns the
    /// display-only DEFAULT_LANG_PROFILE stub, which is no entry in that list).
    [[nodiscard]] auto GetActiveProfileIndex() const -> std::uint32_t
    {
        return m_activatedProfile < m_langProfiles.size() ? m_activatedProfile : UINT32_MAX;
    }

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
    std::string                          m_configuredProfile;  ///< [core] default_input_method, IME thread only.
    bool                                 m_reapplyOnEnable = true; ///< [core] reapply_default_input_method_on_enable
    bool                                 m_pendingApply   = false; ///< the pick changed: one apply is owed even with reapply off
    InputMethodIdentity::Identity        m_configuredIdentity; ///< parsed m_configuredProfile.
    std::function<void()>                m_conversionModeRefresher = nullptr;
};
} // namespace Ime

#endif
