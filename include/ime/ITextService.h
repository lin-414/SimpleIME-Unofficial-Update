//
// Created by jamie on 2025/2/21.
//

#pragma once

#include "CandidateUi.h"
#include "TextEditor.h"
#include "core/State.h"

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

    virtual void RegisterCallback(OnEndCompositionCallback *callback) { m_OnEndCompositionCallback = callback; }

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