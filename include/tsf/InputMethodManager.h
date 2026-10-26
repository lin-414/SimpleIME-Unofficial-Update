#ifndef TSF_LANGPROFILEUTIL_H
#define TSF_LANGPROFILEUTIL_H

#pragma once

#include "LangProfile.h"
#include "TsfSupport.h"
#include "core/State.h"
#include "ime/InputMethodChoice.h"
#include "utils/InputMethodIdentity.h"

#include <atomic>
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
    /// Activate an installed keyboard layout by KLID ("00000409", 8 hex digits —
    /// the form InputMethodIdentity stores as keyboard:{KLID}). Loads the layout
    /// through LoadKeyboardLayout first, so a layout that is installed but not
    /// enumerated for this process still activates. Also forces the layout at the
    /// Win32 level (ActivateKeyboardLayout/KLF_SETFORPROCESS), which is what
    /// actually switches the process input method for KEYBOARDLAYOUT profiles.
    /// The activation is treated as self-initiated (see m_lastUserProfile): it
    /// never overwrites the input method the user picked.
    auto ActivateKeyboardProfile(const std::string &klid) -> HRESULT;
    /// Activate the user's first non-English TIP (input processor) from the
    /// enumerated profile list. Fallback for re-enabling the IME when nothing
    /// was remembered (see HasRememberedUserProfile): at that point the current
    /// TIP is usually the English keyboard left over from an earlier disable,
    /// and silently keeping it means typing produces English only.
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
    /// True when a usable identity is configured: a TIP (tip: identity or a
    /// display name) or a keyboard layout (keyboard:{KLID}).
    [[nodiscard]] auto HasConfiguredProfile() const -> bool;
    /// True when the profile reported active for this thread already is the
    /// configured one — callers use it to skip a redundant activation.
    [[nodiscard]] auto IsConfiguredProfileActive() const -> bool;
    /// Activate the configured input method. Returns E_INVALIDARG (and logs the
    /// raw value, plus one in-game toast per configured value) when it is not
    /// installed/enabled; the caller owns the fallback decision — this never
    /// falls back on its own.
    auto ActivateConfiguredProfile() -> HRESULT;

    /// Every selectable input method of this process — TSF input processors and
    /// plain keyboard layouts (the latter as keyboard:{KLID}, so "stay on the
    /// English layout in this session" is a real choice) — sorted by display
    /// name; the source of the settings-window picker. A layout whose KLID could
    /// not be resolved is skipped: it has no identity to store. Reads the cached
    /// enumeration; call @c RefreshProfiles first when the caller wants a fresh
    /// list.
    [[nodiscard]] auto GetSelectableChoices() const -> std::vector<InputMethodChoice>;

private:
    auto ActivateProfile(const LangProfile &langProfile) -> HRESULT;
    /// Activate the profile named by an identity: a TIP matched against the
    /// enumeration, a keyboard layout by KLID. E_INVALIDARG when nothing matches
    /// (never falls back on its own).
    auto ActivateIdentity(const InputMethodIdentity::Identity &identity) -> HRESULT;

public:
    auto GetActiveLangProfile() -> const LangProfile &;
    /// Index of the active profile inside GetLangProfiles(), or UINT32_MAX when
    /// the cached index is stale (GetActiveLangProfile() then returns the
    /// display-only DEFAULT_LANG_PROFILE stub, which is no entry in that list).
    [[nodiscard]] auto GetActiveProfileIndex() const -> std::uint32_t
    {
        return m_activatedProfile < m_langProfiles.size() ? m_activatedProfile : UINT32_MAX;
    }

    /// The last input method the USER picked in this process — from the
    /// language bar, Win+Space, or the settings window: a TIP (WeChat/Microsoft
    /// Pinyin) or a keyboard layout (美式键盘). Activations this manager performs
    /// itself (the disable path's English keyboard, the configured default, the
    /// "no TIP active" fallback) are NOT recorded, so the memory always answers
    /// "what was the user typing with". Unlike the active-profile cache it is not
    /// clobbered by the English layout the disable path switches to — that used
    /// to lose the restore — and unlike a TIP-only memory it lets a manual
    /// 美式键盘 pick survive an IME enable.
    [[nodiscard]] auto HasRememberedUserProfile() const -> bool;
    /// Re-activate the profile from HasRememberedUserProfile(). E_INVALIDARG
    /// (and the memory is dropped) when it is no longer installed.
    auto RestoreRememberedUserProfile() -> HRESULT;

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

    /// Marks a stretch of work as "this manager switches the profile on its own":
    /// the activation sink is SYNCHRONOUS (the log proves it — the sink line lands
    /// before ActivateProfile logs its return), so an activation recorded while a
    /// pause is held is this mod's own switch, not a user pick.
    auto BeginSelfActivation() -> void;
    auto EndSelfActivation() -> void;
    /// RAII form of the pair above; nest-safe through the counter.
    class ScopedSelfActivation
    {
    public:
        explicit ScopedSelfActivation(InputMethodManager &manager) : m_manager(manager) { m_manager.BeginSelfActivation(); }
        ~ScopedSelfActivation() { m_manager.EndSelfActivation(); }
        ScopedSelfActivation(const ScopedSelfActivation &)            = delete;
        ScopedSelfActivation &operator=(const ScopedSelfActivation &) = delete;

    private:
        InputMethodManager &m_manager;
    };

    /// Whether the profile named by @p identity already is the cached active one.
    [[nodiscard]] auto IsIdentityActive(const InputMethodIdentity::Identity &identity) const -> bool;

    std::vector<LangProfile>             m_langProfiles;
    CComPtr<ITfInputProcessorProfileMgr> m_tfProfileMgr = nullptr;
    CComPtr<ITfThreadMgr>                m_threadMgr    = nullptr;
    TfClientId                           m_clientId;
    // Starts at 1: the constructing ImeWnd owns the initial reference. The
    // TSF AdviseSink reference is the only other one, so UnadviseSink can
    // never drop the count to 0 mid-UnInitialize (the self-delete used to run
    // the dtor — re-entering UnInitialize — while the outer frame kept using
    // the freed members); the owner's final Release is what deletes the
    // object.
    DWORD                                m_refCount{1};
    DWORD                                m_dwCookie{};
    uint32_t                             m_activatedProfile = 0;
    /// Last input method the user picked; see HasRememberedUserProfile().
    InputMethodIdentity::Identity        m_lastUserProfile;
    /// Non-zero while this manager activates a profile on its own: the
    /// activation sink is SYNCHRONOUS (observed in the log: the sink line lands
    /// before ActivateProfile returns), so a plain flag around the TSF call
    /// suffices. A counter, not a bool, so nested self-initiated switches stay
    /// correct.
    /// Atomic: the game-thread English switch (ImeManager::DoDisable) pauses it
    /// too, while the sink reads it on the IME thread.
    std::atomic<int>                     m_suppressUserTracking{0};
    std::string                          m_configuredProfile;  ///< [core] default_input_method, IME thread only.
    bool                                 m_reapplyOnEnable = true; ///< [core] reapply_default_input_method_on_enable
    bool                                 m_pendingApply   = false; ///< the pick changed: one apply is owed even with reapply off
    bool                                 m_unavailableNotified = false; ///< one "not available" toast per configured value
    InputMethodIdentity::Identity        m_configuredIdentity; ///< parsed m_configuredProfile.
    std::function<void()>                m_conversionModeRefresher = nullptr;
};
} // namespace Ime

#endif
