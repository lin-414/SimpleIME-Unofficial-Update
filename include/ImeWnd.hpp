#ifndef IMEWND_HPP
#define IMEWND_HPP

#pragma once

#include "core/State.h"
#include "ime/ITextService.h"
#include "tsf/InputMethodManager.h"
#include "ui/ImeOverlay.h"
#include "ui/ImeWindow.h"

#include "atlcomcli_shim.h"
#include <atomic>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace ImGuiEx::M3
{
class M3Styles;
}

namespace Ime
{
/// `static inline` would give this internal linkage — one copy per translation
/// unit — so it is plain `inline`: a single, shared definition.
inline auto      g_MainClassName                 = L"SimpleIME";
static constexpr size_t TRANSLATOR_DEBOUNCE_DELAY_SECONDS = 5LLU;

class ImeWnd
{
    using State = Core::State;

public:
    ImeWnd() = default;
    ~ImeWnd();

    ImeWnd(ImeWnd &&a_imeWnd)                 = delete;
    ImeWnd(const ImeWnd &a_imeWnd)            = delete;
    ImeWnd &operator=(ImeWnd &&a_imeWnd)      = delete;
    ImeWnd &operator=(const ImeWnd &a_imeWnd) = delete;

    void Initialize(bool enableTsf) noexcept(false);
    void UnInitialize() noexcept;

    static void Run();

    /**
     * Work on standalone thread and run own message loop.
     * Mainly avoid other plugins that init COM
     * with COINIT_MULTITHREADED(crash logger) to affect our TSF code.
     *
     * @param hWndParent Main window (game window)
     * @param settings @Settings
     */
    void CreateHost(HWND hWndParent, Settings &settings);

    auto Focus() const -> void;
    auto FocusTextService(bool focus) const -> bool;
    auto ToggleKeyboard(bool open) const -> void;
    auto IsFocused() const -> bool;
    auto SendNotifyMessageToIme(UINT uMsg, WPARAM wparam, LPARAM lparam) const -> bool;

    //! Must call from IME thread.
    //! @see ImeController::CommitCandidate
    [[nodiscard]] bool CommitCandidate(const DWORD index) const
    {
        return m_textService != nullptr && m_textService->CommitCandidate(index);
    }

    //! Must call from IME thread.
    //! @see ImeController::SetConversionMode
    void SetConversionMode(const DWORD conversionMode) const
    {
        if (m_textService)
        {
            m_textService->SetConversionMode(conversionMode);
        }
    }

    //! Must call from IME thread. Ask the IME to move its candidate window by
    //! `delta` pages (+1 next, -1 previous). Best-effort: a TIP may refuse.
    //! @see ImeController::ChangeCandidatePage
    void ChangeCandidatePage(const int delta) const
    {
        if (m_textService)
        {
            m_textService->ChangeCandidatePage(delta);
        }
    }

    //! Must call from IME thread. Move the highlighted candidate by `delta`
    //! (+1 next, -1 previous) inside the current page. The TSF keyboard channel
    //! is intentionally disabled (see ImeWnd::Run), so this is applied to the
    //! cached candidate list locally.
    void MoveCandidateSelection(const int delta) const
    {
        if (m_textService)
        {
            m_textService->MoveCandidateSelection(delta);
        }
    }

    //! Commit the candidate currently highlighted in the self-drawn list.
    //! Must call from IME thread.
    void CommitSelectedCandidate() const
    {
        if (m_textService)
        {
            m_textService->CommitSelectedCandidate();
        }
    }

    /**
     * Must call from IME thread. Put the user's active input method into its
     * English state (or back into its native state) without changing the
     * session's language profile.
     *
     * @return false when there is no such channel (Imm32 backend, or the global
     * compartments were unavailable at TSF init) — the caller must then fall
     * back to activating the English keyboard profile.
     */
    [[nodiscard]] auto SetSessionImeEnabled(const bool enabled) const -> bool
    {
        return m_textService != nullptr && m_textService->SetSessionImeEnabled(enabled);
    }

    //! Must call from IME thread.
    //! @see ImeController::ActivateLangProfile
    auto ActivateLanguageProfile(const GUID &guidProfile) const -> HRESULT;

    //! Must call from IME thread. Activate by position in the profile list —
    //! the only way to address a keyboard layout (they all share GUID_NULL).
    auto ActivateLanguageProfileByIndex(const std::uint32_t index) const -> HRESULT
    {
        return m_inputMethodManager != nullptr ? m_inputMethodManager->ActivateProfileByIndex(index) : E_FAIL;
    }

    //! Index of the active profile in GetLangProfiles(), UINT32_MAX when unknown.
    [[nodiscard]] auto GetActiveProfileIndex() const -> std::uint32_t
    {
        return m_inputMethodManager != nullptr ? m_inputMethodManager->GetActiveProfileIndex() : UINT32_MAX;
    }

    //! Must call from IME thread.
    auto GetActiveLangProfile() const -> const LangProfile & { return m_inputMethodManager->GetActiveLangProfile(); }

    //! Whether the user's last pick (a TIP or a keyboard layout) is remembered.
    [[nodiscard]] auto HasRememberedInputMethod() const -> bool
    {
        return m_inputMethodManager != nullptr && m_inputMethodManager->HasRememberedUserProfile();
    }

    //! Must call from IME thread. Re-activate the input method the user last
    //! picked; S_FALSE when nothing was remembered, a failure clears the memory
    //! so the caller's fallback decides.
    auto RestoreRememberedInputMethod() const -> HRESULT
    {
        return m_inputMethodManager != nullptr ? m_inputMethodManager->RestoreRememberedUserProfile() : S_FALSE;
    }

    //! Must call from IME thread. Switch to the English keyboard (langid 0x409).
    auto ActivateEnglishProfile() const -> HRESULT { return m_inputMethodManager->ActivateKeyboardEng(); }

    //! Must call from IME thread. Activate the user's first non-English TIP (fallback when nothing was remembered).
    auto ActivatePreferredImeProfile() const -> HRESULT { return m_inputMethodManager->ActivatePreferredImeProfile(); }

    //! Must call from IME thread. Remember the user's configured default input
    //! method ([core] default_input_method). Takes a copy: Settings belongs to the
    //! render thread, and a std::string must never be shared across threads here.
    void SetConfiguredInputMethod(std::string configuredProfile, const bool reapplyOnEnable) const
    {
        if (m_inputMethodManager != nullptr)
        {
            m_inputMethodManager->SetConfiguredProfile(std::move(configuredProfile), reapplyOnEnable);
        }
    }

    //! Whether the configured default input method should be selected right now:
    //! always with the re-apply policy on, otherwise only once after the pick
    //! changed in the settings window.
    [[nodiscard]] auto ShouldApplyConfiguredInputMethod() const -> bool
    {
        return m_inputMethodManager != nullptr && m_inputMethodManager->ShouldApplyConfiguredProfile();
    }

    //! Whether a usable default input method is configured.
    [[nodiscard]] auto HasConfiguredInputMethod() const -> bool
    {
        return m_inputMethodManager != nullptr && m_inputMethodManager->HasConfiguredProfile();
    }

    //! Whether the configured default input method already is the active one.
    [[nodiscard]] auto IsConfiguredInputMethodActive() const -> bool
    {
        return m_inputMethodManager != nullptr && m_inputMethodManager->IsConfiguredProfileActive();
    }

    //! Must call from IME thread. Activate the configured default input method;
    //! the caller owns the fallback decision (see ImeManager).
    auto ActivateConfiguredInputMethod() const -> HRESULT
    {
        return m_inputMethodManager != nullptr ? m_inputMethodManager->ActivateConfiguredProfile() : E_FAIL;
    }

    //! Must call from IME thread. The selectable input methods for the
    //! settings-window picker; `refresh` re-reads the TSF profile enumeration
    //! first (once per settings-window open — the enumeration touches the
    //! registry for every profile, so it is not free).
    [[nodiscard]] auto GetSelectableInputMethods(const bool refresh) const -> std::vector<InputMethodChoice>
    {
        if (m_inputMethodManager == nullptr) return {};
        if (refresh) (void)m_inputMethodManager->RefreshProfiles();
        return m_inputMethodManager->GetSelectableChoices();
    }

    auto GetHWND() const -> HWND { return m_hWnd; }

    /**
     * Service backing this window, for thread-safe snapshots from non-render
     * threads (MeridianBridge::Tick). Null once teardown started; the snapshot
     * methods themselves lock the service mutex, so a game-thread call running
     * next to the render thread's Draw() is safe.
     */
    [[nodiscard]] auto GetTextService() const -> ITextService *
    {
        return (m_fTearingDown.load(std::memory_order_acquire) || m_textService == nullptr) ? nullptr : m_textService.get();
    }

    /**
     * Focus to a parent window to abort IME
     */
    void AbortIme() const;
    void Draw(Settings &settings);

private:
    static auto WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) -> LRESULT;
    static auto OnNccCreate(HWND hWnd, LPCREATESTRUCT lpCreateStruct) -> LRESULT;

    auto OnCreated(Settings &settings) -> void;

    [[nodiscard]] auto OnDestroy() -> LRESULT;

    void InitializeTextService();
    void DrawImeStates();

    /// The SimpleIME.ReclaimImeFocus.v2 registered message id (0 when the
    /// registration failed); shared by the WndProc dispatch and the
    /// WM_KILLFOCUS re-post.
    static auto ReclaimImeFocusMessage() -> UINT;
    /// Reclaim-message body: pull Win32 focus back from the game window when a
    /// web-bridge session still owns input. Every guard re-checks here — the
    /// post is deferred one hop out of the focus transition.
    auto ReclaimImeFocus(HWND hWnd) -> void;
    /// WM_KEYDOWN body: Shift-tap arming and the Prisma edit-key forward.
    auto HandleKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam) -> void;
    /// WM_KEYUP body: arms the deferred bare-Shift 中/英 prediction and the forward.
    auto HandleKeyUp(UINT uMsg, WPARAM wParam, LPARAM lParam) -> void;
    /// The guards the bare-Shift prediction shares between arming and applying.
    [[nodiscard]] auto IsShiftTapPredictable() const -> bool;
    /// WM_TIMER body: applies the bare-Shift prediction unless the IME published
    /// its own toggle in the meantime.
    auto OnShiftTapPredictTimer() -> void;
    /// WM_CHAR body: the IME gate, the composition/commit echo suppressions and
    /// the Scaleform forwarding. Always consumes the message.
    auto HandleCharMessage(WPARAM wParam, LPARAM lParam) -> LRESULT;

    DebounceTimer                   m_translatorLoadDebounceTimer{std::chrono::seconds(TRANSLATOR_DEBOUNCE_DELAY_SECONDS)};
    std::unique_ptr<ImeWindow>      m_imeWindow             = nullptr;
    std::unique_ptr<UI::ImeOverlay> m_imeOverlay            = nullptr;
    std::unique_ptr<ITextService>   m_textService           = nullptr;
    CComPtr<InputMethodManager>     m_inputMethodManager    = nullptr;
    HWND                            m_hWnd                  = nullptr;
    HWND                            m_hWndParent            = nullptr;
    DWORD                           m_gameThreadId          = 0;
    /// Written by the IME thread on WM_SETTINGCHANGE / WM_DPICHANGED, read by the
    /// render thread in Draw() — hence the atomics. The flag is consumed with
    /// exchange so a DPI change arriving during a frame is not lost.
    std::atomic<float>              m_uiScale               = 1.0F;
    std::atomic<bool>               m_fWantUpdateUiScale    = true; ///< update scale in the first frame.
    bool                            m_fFocused              = false;
    /// Set by WM_KILLFOCUS (IME thread), consumed by Draw() (render thread).
    /// ImGui is single-threaded and the render thread owns the IO — clearing
    /// input keys directly from the WndProc raced the frame in progress.
    ///
    /// The keys must be cleared because this window is the only path that feeds
    /// ImGui's keyboard state (ImeMenu forwards GFx key events), and no key-up
    /// is ever delivered for a key that was still held when the focus left: the
    /// release goes to whatever window took the focus. Without this, ImGui keeps
    /// that key (Ctrl in particular) down forever.
    std::atomic<bool>               m_fWantClearInput       = false;
    /// Set once by OnDestroy() as a teardown latch (also by UnInitialize(), which
    /// it calls) and read by Draw() (render thread).
    /// Teardown destroys m_imeOverlay and clears the profile list that Draw()
    /// iterates, so the render thread must stop touching this object first. The
    /// ImeApp state only leaves INITIALIZED after teardown completes, so without
    /// this flag Draw() can still be running while the IME thread is mid-teardown.
    std::atomic<bool>               m_fTearingDown          = false;
    bool                            m_fEnabledTsf           = true;
    /// GetTickCount64() at the last composition commit, written by the commit
    /// callback and read by the WM_CHAR branch — IME thread only. The commit
    /// keystroke's own WM_CHAR echo is dispatched after the composition (and
    /// IN_COMPOSING) is already gone, so the composing gate cannot catch it;
    /// the WM_CHAR branch drops the first echo inside a small window instead.
    std::uint64_t                   m_lastCommitTickMs      = 0;
    /// Bare-Shift-tap tracker for the optimistic 中/英 toggle prediction
    /// (WeChat IME publishes its mode through no TSF compartment, so the
    /// displayed state is predicted from the Shift tap and corrected by the
    /// behavioral inference on the next keystroke). IME thread only.
    bool                            m_shiftTapArmed         = false;
    std::uint64_t                   m_shiftTapDownTickMs    = 0;
    /// The deferred prediction (WM_TIMER) and the IME's conversion-mode revision
    /// captured at the Shift keydown: a change means the IME published its own
    /// toggle during the tap, and its value then wins (see OnShiftTapPredictTimer).
    bool                            m_shiftTapPredictPending      = false;
    std::uint64_t                   m_shiftTapRevisionAtKeyDown   = 0;
};
} // namespace Ime

#endif
