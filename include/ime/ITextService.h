//
// Created by jamie on 2025/2/21.
//

#pragma once

#include "CandidateUi.h"
#include "TextEditor.h"
#include "core/State.h"

#include <cstdint>
#include <shared_mutex>

namespace Ime
{
using OnEndCompositionCallback = void(std::wstring_view compositionString);

struct CompositionInfo
{
    std::wstring documentText;
    size_t       caretPos{0};
};

class ITextService
{
public:
    enum class DirtyFlag : uint8_t
    {
        None               = 0U,       ///< All changes are clean, no need to update candidate UI.
        CandidateSelection = 1U << 1U, ///< candidate selection changed.
        CandidateList      = 1U << 2U, ///< candidate list changed, selection may also be changed.
        Composition        = 1U << 4U, ///< composition string changed.
        All                = CandidateList | CandidateSelection | Composition
    };

    friend constexpr auto operator|(DirtyFlag lhs, DirtyFlag rhs) -> DirtyFlag
    {
        using Underlying = std::underlying_type_t<DirtyFlag>;
        return static_cast<DirtyFlag>(static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs));
    }

    friend constexpr auto operator&(DirtyFlag lhs, DirtyFlag rhs) -> DirtyFlag
    {
        using Underlying = std::underlying_type_t<DirtyFlag>;
        return static_cast<DirtyFlag>(static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs));
    }

    friend constexpr auto operator|=(DirtyFlag &lhs, DirtyFlag rhs) -> DirtyFlag &
    {
        lhs = lhs | rhs;
        return lhs;
    }

    static constexpr auto HasDirtyFlag(DirtyFlag flag, DirtyFlag rhs) -> bool { return (flag & rhs) != DirtyFlag::None; }

    ITextService()                                                  = default;
    virtual ~ITextService()                                         = default;
    ITextService(const ITextService &other)                         = delete;
    ITextService(ITextService &&other) noexcept                     = delete;
    auto operator=(const ITextService &other) -> ITextService &     = delete;
    auto operator=(ITextService &&other) noexcept -> ITextService & = delete;

    virtual void UnInitialize() {}

    virtual void OnStart([[maybe_unused]] HWND hWnd) {}

    virtual auto OnFocus([[maybe_unused]] bool focus) -> bool { return true; }

    /**
     * @brief Cancel the active composition and clear input state without committing the current composition text.
     *
     * Called from the IME thread when the user clicks outside the input area (AbortIme).
     * The default implementation is a no-op; subclasses (TSF TextService / Imm32TextService)
     * shall terminate the composition and clear IN_COMPOSING / IN_CAND_CHOOSING state flags.
     */
    virtual void AbortIme() {}

    virtual auto ToogleKeyboard(bool open) -> void = 0;

    /**
     * @brief Put the ACTIVE input method into its English state (or back into its
     * native state) without changing the session's language profile.
     *
     * The disable path needs the TIP to stop eating the game's keystrokes. The
     * way that used to do it was activating the English keyboard profile, which
     * is a change to the session-wide input locale: the taskbar indicator went
     * to ENG and stayed there after the game exited, because nothing restores
     * another process's input locale. These cross-process global compartments
     * only move the IME's own open/close + conversion state, which the user's
     * own toggle (Shift) restores.
     *
     * @return false when the backend has no such channel, so the caller can fall
     * back to the profile switch.
     */
    virtual auto SetSessionImeEnabled([[maybe_unused]] bool enabled) -> bool { return false; }

    virtual auto ProcessImeMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> bool = 0;

    /**
     * @brief Update the composition info and candidate info if they are dirty.
     * UI thread shall call this method in an appropriate time, for example, at the beginning of each UI loop, to check if there is any update from
     * TextStore/Imm32 and read the latest composition info and candidate info.
     */
    auto UpdateIfDirty() -> void
    {
        const auto flag = m_dirtyFlag.exchange(DirtyFlag::None, std::memory_order_acq_rel);
        if (flag != DirtyFlag::None)
        {
            RequestUpdate(m_compositionInfo, m_candidateUi, flag);
        }
    }

    [[nodiscard]] auto GetCompositionInfo() const -> const CompositionInfo & { return m_compositionInfo; }

    [[nodiscard]] auto GetCandidateUi() const -> const CandidateUi & { return m_candidateUi; }

    /**
     * @brief Thread-safe point-in-time copy of the composition text and candidate list.
     *
     * For consumers outside the render thread (the Meridian bridge's game-thread
     * Tick). GetCompositionInfo/GetCandidateUi return references the render
     * thread alone may read between UpdateIfDirty calls — RequestUpdate writes
     * them unlocked with respect to any other reader. This method instead copies
     * under the service mutex straight from the live editor state, so calling it
     * concurrently with the render-thread frame is safe.
     */
    virtual auto SnapshotCompositionAndCandidates(CompositionInfo &outComposition, CandidateUi &outCandidates) -> void = 0;

    /**
     * @brief Mark the composition or candidate info dirty.
     * The next time when UI thread calls `UpdateIfDirty`, it will read the latest composition info and candidate info.
     * UI thread shall not call this method directly, it is designed for TextStore/Imm32 to mark the dirty flag when text or candidate updated. UI
     * thread will call `UpdateIfDirty` to check if there is any update and read the latest composition info and candidate info.
     * @param dirtyFlag the flag to indicate which info is dirty, can be combined with bitwise OR operator.
     */
    void MarkDirty(DirtyFlag dirtyFlag)
    {
        auto current = m_dirtyFlag.load(std::memory_order_relaxed);
        while (!m_dirtyFlag.compare_exchange_weak(current, current | dirtyFlag, std::memory_order_release, std::memory_order_relaxed))
        {
        }
    }

    virtual auto CommitCandidate(DWORD index) -> bool            = 0;
    virtual auto SetConversionMode(DWORD conversionMode) -> bool = 0;

    /**
     * @brief Ask the active IME to move its candidate window by `delta` pages.
     *
     * The ImGui candidate window mirrors the TIP's own candidate UI: the TIP
     * owns the paging state, so a page change must be requested from it rather
     * than simulated locally. The TSF backend forwards the PageUp/PageDown
     * keystroke to the TIP (ITfIntegratableCandidateListUIElement::OnKeyDown,
     * falling back to ITfKeystrokeMgr); the Imm32 backend notifies the input
     * context (NI_SETCANDIDATE_PAGESTART).
     *
     * Runs on the IME thread. Returns true when the IME accepted the request;
     * a false return means no page change may be assumed.
     *
     * @param delta +1 for the next page, -1 for the previous page.
     */
    virtual auto ChangeCandidatePage([[maybe_unused]] int delta) -> bool { return false; }

    /**
     * @brief Move the highlighted candidate inside the current page.
     *
     * The self-drawn candidate list has no TSF keyboard channel (ImeWnd's
     * message loop deliberately stays a plain PeekMessage loop — see
     * ImeWnd::Run), so arrow-key navigation is applied to the cached
     * CandidateUi here instead of being forwarded to the TIP. Commit paths
     * (click / Space / Enter) resolve the highlighted index through the same
     * cache.
     *
     * Runs on the IME thread. Returns true when the selection moved.
     *
     * @param delta +1 for the next candidate, -1 for the previous one.
     */
    virtual auto MoveCandidateSelection([[maybe_unused]] int delta) -> bool { return false; }

    /**
     * @brief Commit the candidate currently highlighted in the self-drawn list.
     *
     * The standard IME meaning of Space while a candidate list is up: pick the
     * highlighted entry. The highlight is moved locally by
     * MoveCandidateSelection, so the commit has to resolve it from the same
     * cache rather than from the TIP.
     *
     * Runs on the IME thread. Returns true when a commit was initiated.
     */
    virtual auto CommitSelectedCandidate() -> bool { return false; }

    /**
     * @brief Forward one physical key to the active IME (TSF keystroke channel).
     *
     * While a composition owns the keyboard the engine's input stream is muted
     * (Hooks::InputSuppression) AND the TSF keyboard channel is disabled by
     * design (ImeWnd::Run), so the keys the IME itself edits with — Left/Right
     * moving the pinyin caret, Backspace deleting a pinyin letter — would
     * otherwise reach nobody. This hands exactly those keys to the TIP.
     *
     * Runs on the IME thread. Returns true when the TIP consumed the key.
     */
    virtual auto ForwardKeyToTip([[maybe_unused]] UINT vk, [[maybe_unused]] LPARAM lParam) -> bool { return false; }

    /**
     * @brief Re-read the active conversion mode from the IME into State.
     *
     * Called when an input processor activates (profile switch) so the UI
     * re-seeds from a live source. The TSF implementation prefers the focused
     * context's conversion compartment (the live value for WeChat IME) and
     * falls back to the thread-level compartment; the Imm32 implementation
     * tracks the mode via IMN_SETCONVERSIONMODE notifications instead, so the
     * default no-op is correct there.
     */
    virtual auto RefreshConversionMode() -> void {}

    /**
     * @brief Monotonic counter, bumped every time the active IME PUBLISHES a
     * conversion mode through a TSF compartment.
     *
     * The bare-Shift 中/英 prediction needs to know whether the IME handled the
     * tap itself, and the published VALUE cannot answer that: Microsoft Pinyin
     * writes its toggle from the same WM_KEYUP that arms the prediction, so by
     * the time the deferred step looks, the value is the post-toggle one either
     * way. A changed revision is the unambiguous signal — "the IME published
     * during this tap, so its value is the truth". IMEs that publish nothing at
     * all (WeChat) leave it untouched, which is exactly when the prediction
     * must step in.
     *
     * The Imm32 backend tracks the mode through IMN_SETCONVERSIONMODE
     * notifications rather than compartments, so it keeps the default 0 and the
     * prediction stays unconditional there (the pre-existing behavior).
     */
    [[nodiscard]] virtual auto ConversionModeRevision() const -> std::uint64_t { return 0; }

    /**
     * @brief Remember the live conversion mode as the one a NEW text-entry
     * session restores ("the mode text was last confirmed in").
     *
     * Called at every text confirmation — a composition commit, a
     * composition-less insert, a forwarded WM_CHAR character — so the memory
     * follows what the user is actually typing, not what the compartments hold
     * during the session teardown (sampling there is what brought a Japanese IME
     * back in ひらがな on every console open). The TSF backend reads the focused
     * context's compartment, falls back to the thread one and finally to State
     * (IMEs that publish nothing are tracked by behavioral inference).
     *
     * const because it reads the service and writes the State singleton, nothing
     * else — that is what lets the callers reach it through ImeWnd's
     * teardown-guarded GetTextService() (which hands out a const pointer).
     */
    virtual auto StampRememberedConversionModeFromLive() const -> void {}

    virtual void RegisterCallback(OnEndCompositionCallback *callback) { m_OnEndCompositionCallback = callback; }

    /// 本次组字期间引擎同时读到的原始字符数（TSF 后端从组字内容取；Imm32 后端不
    /// 维护该量，返回 0 即"不退格"）。见 TextStore::GetComposingLength。
    [[nodiscard]] virtual auto GetComposingLength() const -> std::size_t { return 0; }

private:
    CompositionInfo        m_compositionInfo{};
    CandidateUi            m_candidateUi;
    std::atomic<DirtyFlag> m_dirtyFlag{DirtyFlag::All};

protected:
    OnEndCompositionCallback *m_OnEndCompositionCallback = nullptr;

    virtual void RequestUpdate(CompositionInfo &compositionInfo, CandidateUi &uiForRead, DirtyFlag flag) = 0;
};

namespace Imm32
{
class Imm32TextService final : public ITextService
{
public:
    Imm32TextService()                                                      = default;
    Imm32TextService(const Imm32TextService &other)                         = delete;
    Imm32TextService(Imm32TextService &&other) noexcept                     = delete;
    auto operator=(const Imm32TextService &other) -> Imm32TextService &     = delete;
    auto operator=(Imm32TextService &&other) noexcept -> Imm32TextService & = delete;

    /**
     * return true means message has processed.
     */
    auto ProcessImeMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> bool override;

    void OnStart(HWND hWnd) override { m_imeHwnd = hWnd; }

    bool OnFocus(bool focus) override;

    auto ToogleKeyboard(bool open) -> void override;

    auto CommitCandidate(DWORD index) -> bool override;

    auto SetConversionMode(DWORD conversionMode) -> bool override;

    auto ChangeCandidatePage(int delta) -> bool override;

    auto MoveCandidateSelection(int delta) -> bool override;

    auto CommitSelectedCandidate() -> bool override;

    /// See ITextService::StampRememberedConversionModeFromLive. There are no
    /// compartments on this backend: the live mode is the input context's own
    /// conversion status. Without this override the inherited no-op left the
    /// remembered mode at its startup seed forever (nothing else ever writes it
    /// on this backend), so input.last_native_conversion stopped following the
    /// user and the per-cycle capture fallback in SetSessionImeEnabled became
    /// unreachable.
    auto StampRememberedConversionModeFromLive() const -> void override;

    auto SnapshotCompositionAndCandidates(CompositionInfo &outComposition, CandidateUi &outCandidates) -> void override
    {
        const std::scoped_lock lock(m_mutex);
        outComposition.documentText = m_textEditor.GetText();
        outComposition.caretPos     = m_textEditor.GetStart();
        outCandidates               = m_candidateUi;
    }

    void AbortIme() override;

protected:
    void RequestUpdate(CompositionInfo &compositionInfo, CandidateUi &uiForRead, DirtyFlag flag) override
    {
        const std::scoped_lock lock(m_mutex);
        if (HasDirtyFlag(flag, DirtyFlag::Composition))
        {
            compositionInfo.documentText = m_textEditor.GetText();
            compositionInfo.caretPos     = m_textEditor.GetStart();
        }
        if (HasDirtyFlag(flag, DirtyFlag::CandidateSelection) || HasDirtyFlag(flag, DirtyFlag::CandidateList))
        {
            // Copy, not swap: this member is the live state the IME thread
            // writes and SnapshotCompositionAndCandidates reads; swapping
            // would leave it holding the previous generation.
            uiForRead = m_candidateUi;
        }
    }

private:
    static void OnStartComposition();
    void        OnEndComposition();
    void        OnComposition(HWND hWnd, LPARAM compFlag);
    void        UpdateComposition(std::wstring_view compStr, size_t cursorPos, size_t deltaStart);
    auto        OnImeNotify(HWND hWnd, WPARAM wParam, LPARAM lParam) -> void;

    void OpenCandidate(HIMC hIMC);
    void ChangeCandidate(HIMC hIMC);
    void ChangeCandidateAt(HIMC hIMC);
    void DoUpdateCandidateList(LPCANDIDATELIST lpCandList);

    TextEditor                m_textEditor;
    CandidateUi               m_candidateUi;
    HWND                      m_imeHwnd = nullptr;
    HIMC                      m_hIMC    = nullptr;
    mutable std::shared_mutex m_mutex;
};
} // namespace Imm32
} // namespace Ime