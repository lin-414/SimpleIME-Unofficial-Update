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

    //! Must call from IME thread.
    //! @see ImeController::ActivateLangProfile
    auto ActivateLanguageProfile(const GUID &guidProfile) const -> HRESULT;

    //! Must call from IME thread.
    auto GetActiveLangProfile() const -> const LangProfile & { return m_inputMethodManager->GetActiveLangProfile(); }

    //! GUID of the last real input processor (TIP) that activated — see InputMethodManager.
    [[nodiscard]] auto GetLastTipProfileGuid() const -> const GUID & { return m_inputMethodManager->GetLastTipProfileGuid(); }

    //! Must call from IME thread. Switch to the English keyboard (langid 0x409).
    auto ActivateEnglishProfile() const -> HRESULT { return m_inputMethodManager->ActivateKeyboardEng(); }

    //! Must call from IME thread. Activate the user's first non-English TIP (fallback when nothing was remembered).
    auto ActivatePreferredImeProfile() const -> HRESULT { return m_inputMethodManager->ActivatePreferredImeProfile(); }

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
    /// WM_KEYUP body: the bare-Shift-tap 中/英 prediction and the forward.
    auto HandleKeyUp(UINT uMsg, WPARAM wParam, LPARAM lParam) -> void;
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
};
} // namespace Ime

#endif
