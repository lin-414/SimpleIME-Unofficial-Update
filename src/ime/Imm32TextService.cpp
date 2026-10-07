//
// Created by jamie on 2025/2/21.
//

#include "WCharUtils.h"
#include "configs/CustomMessage.h"
#include "ime/ITextService.h"
#include "log.h"

#include <algorithm>
#include <imm.h>

#pragma comment(lib, "imm32.lib")

namespace Ime::Imm32
{
using State = Ime::Core::State;

namespace
{

void UpdateConversionMode(HIMC hIMC)
{
    DWORD conversion = 0;
    DWORD sentence   = 0;
    if (ImmGetConversionStatus(hIMC, &conversion, &sentence) != FALSE)
    {
        State::GetInstance().SetConversionMode(conversion);
    }
}

inline void UpdateOpenStatus(HIMC hIMC)
{
    State::GetInstance().Set(State::KEYBOARD_OPEN, ImmGetOpenStatus(hIMC) != 0);
}

inline void UpdateOpenStatus(HIMC hIMC, BOOL open)
{
    ImmSetOpenStatus(hIMC, open);
}

auto GetCompStr(HIMC hIMC, LPARAM compFlag, LPARAM flagToCheck, std::wstring &pWcharBuf) -> bool
{
    if ((compFlag & flagToCheck) != 0)
    {
        const LONG bufLenInBytes = ImmGetCompositionStringW(hIMC, static_cast<DWORD>(flagToCheck), nullptr, 0);
        if (bufLenInBytes > 0)
        {
            const auto dwBufLenInBytes = static_cast<DWORD>(bufLenInBytes);
            pWcharBuf.resize(dwBufLenInBytes / sizeof(WCHAR));
            const LONG written = ImmGetCompositionStringW(hIMC, static_cast<DWORD>(flagToCheck), pWcharBuf.data(), dwBufLenInBytes);
            if (written > 0)
            {
                if (written != bufLenInBytes)
                {
                    pWcharBuf.resize(static_cast<size_t>(written) / sizeof(WCHAR));
                }
                return true;
            }
        }
    }
    pWcharBuf.clear();
    return false;
}

} // namespace

void Imm32TextService::OnStartComposition()
{
    State::GetInstance().Set(State::IN_COMPOSING);
    // Behavioral mode inference (mirrors the TSF path): composing implies the
    // native mode bit, whatever the IME reports through its compartments.
    State::GetInstance().AddConversionModeFlag(State::ConversionMode::Flags::NATIVE);
}

void Imm32TextService::OnEndComposition()
{
    {
        const std::scoped_lock lock(m_mutex);
        // Same focus guard as the TSF path (TextStore::OnEndComposition): when
        // the composition ends because an OS window switch stole the focus, do
        // not push the partial text into the game mid-transition.
        if (m_OnEndCompositionCallback != nullptr && GetFocus() == m_imeHwnd)
        {
            m_OnEndCompositionCallback(m_textEditor.GetText());
        }
        m_textEditor.Select(0, 0);
        m_textEditor.ClearText();
        m_candidateUi.Close();
        // Publish the cleared composition/candidate state to the render thread's
        // copy immediately; without this, a new composition starting within the
        // next frames could briefly render the previous candidate list.
        MarkDirty(DirtyFlag::CandidateList);
        MarkDirty(DirtyFlag::Composition);
    }
    State::GetInstance().ClearComposing();
}

void Imm32TextService::AbortIme()
{
    // Clear local state first without calling the callback (no text injection),
    // then cancel the composition at the system level. The WM_IME_ENDCOMPOSITION
    // that CPS_CANCEL triggers will arrive later and find an already-empty editor.
    {
        const std::scoped_lock lock(m_mutex);
        m_textEditor.Select(0, 0);
        m_textEditor.ClearText();
        m_candidateUi.Close();
        // Publish the cleared composition/candidate state to the render thread's
        // copy immediately; without this, a new composition starting within the
        // next frames could briefly render the previous candidate list.
        MarkDirty(DirtyFlag::CandidateList);
        MarkDirty(DirtyFlag::Composition);
    }
    if (m_imeHwnd != nullptr)
    {
        if (HIMC hIMC = ImmGetContext(m_imeHwnd); hIMC != nullptr)
        {
            ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(m_imeHwnd, hIMC);
        }
    }
    State::GetInstance().ClearComposing();
}

auto Imm32TextService::ProcessImeMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> bool
{
    switch (message)
    {
        case WM_CREATE:
            m_hIMC = ImmCreateContext();
            break;
        case WM_DESTROY: {
            ImmAssociateContext(hWnd, nullptr);
            if (m_hIMC != nullptr)
            {
                ImmDestroyContext(m_hIMC);
                m_hIMC = nullptr; // a repeated WM_DESTROY must not destroy it again
            }
            break;
        }
        case WM_IME_STARTCOMPOSITION:
            OnStartComposition();
            return true;
        case WM_IME_ENDCOMPOSITION:
            OnEndComposition();
            return true;
        case CM_IME_COMPOSITION:
        case WM_IME_COMPOSITION:
            OnComposition(hWnd, lParam);
            return true;
        case WM_IME_NOTIFY: {
            OnImeNotify(hWnd, wParam, lParam);
            break;
        }
        default:
            break;
    }

    return false;
}

auto Imm32TextService::OnFocus(bool focus) -> bool
{
    if (focus)
    {
        if (m_hIMC == nullptr)
        {
            m_hIMC = ImmCreateContext();
        }
        ImmAssociateContext(m_imeHwnd, m_hIMC);

        if (HIMC himc = ImmGetContext(m_imeHwnd); himc != nullptr)
        {
            UpdateConversionMode(himc);
            ImmReleaseContext(m_imeHwnd, himc);
        }
    }
    else
    {
        m_hIMC = ImmAssociateContext(m_imeHwnd, nullptr);
    }
    State::GetInstance().Set(State::TEXT_SERVICE_FOCUS, focus);
    return ITextService::OnFocus(focus);
}

auto Imm32TextService::ToogleKeyboard(bool open) -> void
{
    if (HIMC himc = ImmGetContext(m_imeHwnd); himc != nullptr)
    {
        UpdateOpenStatus(himc, open ? TRUE : FALSE);
        UpdateConversionMode(himc);
        ImmReleaseContext(m_imeHwnd, himc);
    }
}

// This method does not work as expected
auto Imm32TextService::CommitCandidate(DWORD index) -> bool
{
    logger::debug("CommitCandidate {}", index);
    HIMC hImc = ImmGetContext(m_imeHwnd);
    if (hImc == nullptr)
    {
        // Without a context the selection can never reach the IME.
        logger::error("CommitCandidate({}) failed: no input context on the IME window", index);
        return false;
    }

    const bool result = ImmNotifyIME(hImc, NI_SELECTCANDIDATESTR, 0, index) != FALSE;
    ImmReleaseContext(m_imeHwnd, hImc);
    return result;
}

auto Imm32TextService::SetConversionMode(DWORD conversionMode) -> bool
{
    HIMC himc    = ImmGetContext(m_imeHwnd);
    bool success = false;
    if (himc != nullptr)
    {
        DWORD oldConversion = 0;
        DWORD oldSentence   = 0;
        if (ImmGetConversionStatus(himc, &oldConversion, &oldSentence) != FALSE)
        {
            success = FALSE != ImmSetConversionStatus(himc, conversionMode, oldSentence);
        }
        ImmReleaseContext(m_imeHwnd, himc);
    }
    return success;
}

void Imm32TextService::OnComposition(HWND hWnd, LPARAM compFlag)
{
    HIMC hIMC = ImmGetContext(hWnd);
    if (hIMC == nullptr)
    {
        return;
    }

    std::wstring compositionSting;
    if (GetCompStr(hIMC, compFlag, GCS_RESULTSTR, compositionSting))
    {
        const std::scoped_lock lock(m_mutex);

        m_textEditor.SelectAll();
        m_textEditor.InsertText(compositionSting);
        MarkDirty(DirtyFlag::Composition);
        if (spdlog::should_log(spdlog::level::trace))
        {
            const auto str = WCharUtils::ToString(compositionSting);
            logger::trace("IME Composition Result String: {}", str.c_str());
        }
    }
    else if (GetCompStr(hIMC, compFlag, GCS_COMPSTR, compositionSting))
    {
        const std::scoped_lock lock(m_mutex);

        const int32_t cursorPos  = ImmGetCompositionStringW(hIMC, GCS_CURSORPOS, nullptr, 0);
        const int32_t deltaStart = ImmGetCompositionStringW(hIMC, GCS_DELTASTART, nullptr, 0);
        // IMM_ERROR_NODATA or IMM_ERROR_GENERAL
        if (cursorPos == IMM_ERROR_GENERAL || deltaStart == IMM_ERROR_GENERAL)
        {
            logger::error("Get composition cursor position or delta start failed.");
            ImmReleaseContext(hWnd, hIMC); // error path must still pair the ImmGetContext above
            return;
        }
        if (cursorPos >= 0 && deltaStart >= 0)
        {
            UpdateComposition(compositionSting, static_cast<size_t>(cursorPos), static_cast<size_t>(deltaStart));
        }
    }
    ImmReleaseContext(hWnd, hIMC);
}

void Imm32TextService::UpdateComposition(std::wstring_view compStr, size_t cursorPos, size_t deltaStart)
{
    const auto prevSize = m_textEditor.GetTextSize();

    deltaStart = std::clamp(deltaStart, 0LLU, prevSize);

    m_textEditor.Select(static_cast<int32_t>(deltaStart), -1);

    deltaStart = std::min(deltaStart, compStr.length());
    m_textEditor.InsertText(compStr.substr(deltaStart));

    cursorPos = std::clamp(cursorPos, 0LLU, m_textEditor.GetTextSize());
    m_textEditor.Select(static_cast<int32_t>(cursorPos), static_cast<int32_t>(cursorPos));

    MarkDirty(DirtyFlag::Composition);
    if (spdlog::should_log(spdlog::level::trace))
    {
        const auto str = WCharUtils::ToString(compStr);
        logger::trace("IME Composition String: {}", str.c_str());
    }
}

auto Imm32TextService::OnImeNotify(HWND hWnd, WPARAM wParam, LPARAM /*lParam*/) -> void
{
    // logger::debug("ImeNotify {:#x}, {:#x}", wParam, lParam);
    switch (wParam)
    {
        case IMN_SETCANDIDATEPOS:
        case IMN_OPENCANDIDATE: {
            State::GetInstance().Set(State::IN_CAND_CHOOSING);
            HIMC hImc = ImmGetContext(hWnd);
            if (hImc != nullptr)
            {
                OpenCandidate(hImc);
                ImmReleaseContext(hWnd, hImc); // fires often during candidate mode — must pair the Get
            }
            break;
        }
        case IMN_CLOSECANDIDATE:
            State::GetInstance().Clear(State::IN_CAND_CHOOSING);
            break;
        case IMN_CHANGECANDIDATE: {
            HIMC hIMC = ImmGetContext(hWnd);
            if (hIMC != nullptr)
            {
                ChangeCandidate(hIMC);
                ImmReleaseContext(hWnd, hIMC);
            }
            break;
        }
        case IMN_SETCONVERSIONMODE: {
            HIMC hIMC = ImmGetContext(hWnd);
            if (hIMC != nullptr)
            {
                UpdateConversionMode(hIMC);
                ImmReleaseContext(hWnd, hIMC);
            }
            break;
        }
        case IMN_SETOPENSTATUS: {
            HIMC hIMC = ImmGetContext(hWnd);
            if (hIMC != nullptr)
            {
                UpdateOpenStatus(hIMC);
                UpdateConversionMode(hIMC);
                ImmReleaseContext(hWnd, hIMC);
            }
            break;
        }
        default:
            break;
    }
}

void Imm32TextService::OpenCandidate(HIMC hIMC)
{
    ChangeCandidateAt(hIMC);
}

void Imm32TextService::ChangeCandidate(HIMC hIMC)
{
    ChangeCandidateAt(hIMC);
}

void Imm32TextService::ChangeCandidateAt(HIMC hIMC)
{
    DWORD bufLen = ImmGetCandidateListW(hIMC, 0, nullptr, 0);
    if (bufLen == 0)
    {
        return;
    }
    HGLOBAL hGlobal = GlobalAlloc(LPTR, bufLen);
    if (hGlobal == nullptr)
    {
        logger::warn("Global alloc {} failed.", bufLen);
        return;
    }
    auto *lpCandList = static_cast<LPCANDIDATELIST>(GlobalLock(hGlobal));
    if (lpCandList == nullptr)
    {
        logger::error("Candidate alloc memory failed.");
        GlobalFree(hGlobal);
        return;
    }
    bufLen = ImmGetCandidateListW(hIMC, 0, lpCandList, bufLen);
    if (bufLen > 0)
    {
        DoUpdateCandidateList(lpCandList);
    }
    GlobalUnlock(hGlobal);
    GlobalFree(hGlobal);
}

void Imm32TextService::DoUpdateCandidateList(LPCANDIDATELIST lpCandList)
{
    DWORD dwStartIndex = lpCandList->dwPageStart;
    DWORD dwEndIndex   = dwStartIndex + lpCandList->dwPageSize;
    dwEndIndex         = std::min(dwEndIndex, lpCandList->dwCount);

    const std::scoped_lock lock(m_mutex);

    m_candidateUi.Close();
    m_candidateUi.Reserve(lpCandList->dwPageSize);
    auto *lpCandListByte = reinterpret_cast<LPCH>(lpCandList);
    for (DWORD index = 0; dwStartIndex < dwEndIndex; ++index, ++dwStartIndex)
    {
        auto                   *pcCandidate = lpCandListByte + lpCandList->dwOffset[dwStartIndex];
        const std::wstring_view wsvCandidate(reinterpret_cast<LPWCH>(pcCandidate));
        const std::string       ansiStr = WCharUtils::ToString(wsvCandidate);
        m_candidateUi.PushBack(std::format("{}. {}", index + 1, ansiStr));
    }
    // dwSelection is a GLOBAL candidate index, but the list we just pushed is
    // page-relative (dwPageStart..dwPageStart+dwPageSize). Subtract the page
    // start — clamped, mirroring the TSF path (TextStore::DoUpdateUIElement) —
    // or the highlight would be misplaced / out of range from page 2 on.
    m_candidateUi.SetSelection(lpCandList->dwSelection > lpCandList->dwPageStart
                                   ? static_cast<DWORD>(lpCandList->dwSelection - lpCandList->dwPageStart)
                                   : 0);
    MarkDirty(DirtyFlag::CandidateList);
}

} // namespace Ime::Imm32
