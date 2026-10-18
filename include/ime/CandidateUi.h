#ifndef IME_IME_H
#define IME_IME_H

#pragma once

#include <list>
#include <string>
#include <vector>
#include <windows.h>

namespace Ime
{
class CandidateUi
{
public:
    using CandidateList_t = std::vector<std::string>;
    using size_type       = CandidateList_t::size_type;

    void Reserve(const DWORD dwPageSize) { m_candidateList.reserve(dwPageSize); }

    //! [internal use] A cache field. Be used to the candidate list is need to update when page up or page down.
    void SetFirstIndex(const DWORD firstIndex) { m_dwFirstIndex = firstIndex; }

    void SetSelection(const DWORD dwSelection) { m_dwSelection = dwSelection; }

    /// Page geometry reported by the IME. TSF fills it from
    /// GetPageIndex/GetCurrentPage/GetCount; Imm32 from CANDIDATELIST
    /// (dwPageSize/dwPageStart/dwCount). The UI only needs it to render the
    /// "‹ 1/5 ›" page control and to hide it for single-page lists.
    void SetPageInfo(const DWORD pageSize, const DWORD currentPage, const DWORD pageCount, const DWORD totalCount)
    {
        m_dwPageSize    = pageSize;
        m_dwCurrentPage = currentPage;
        m_dwPageCount   = pageCount;
        m_dwTotalCount  = totalCount;
    }

    [[nodiscard]] auto FirstIndex() const -> DWORD { return m_dwFirstIndex; }

    [[nodiscard]] auto Selection() const -> DWORD { return m_dwSelection; }

    [[nodiscard]] auto PageSize() const -> DWORD { return m_dwPageSize; }

    [[nodiscard]] auto CurrentPage() const -> DWORD { return m_dwCurrentPage; }

    [[nodiscard]] auto PageCount() const -> DWORD { return m_dwPageCount; }

    [[nodiscard]] auto TotalCount() const -> DWORD { return m_dwTotalCount; }

    /// Returns a reference: the UI calls this every frame, and a by-value
    /// return heap-copied the whole candidate list per frame.
    [[nodiscard]] auto CandidateList() const -> const CandidateList_t & { return m_candidateList; }

    auto PushBack(const std::string &candidate) -> void { m_candidateList.push_back(candidate); }

    auto PushBack(std::string &&candidate) -> void { m_candidateList.emplace_back(std::move(candidate)); }

    auto swap(CandidateUi &right) -> void
    {
        m_candidateList.swap(right.m_candidateList);
        std::swap(m_dwFirstIndex, right.m_dwFirstIndex);
        std::swap(m_dwSelection, right.m_dwSelection);
        std::swap(m_dwPageSize, right.m_dwPageSize);
        std::swap(m_dwCurrentPage, right.m_dwCurrentPage);
        std::swap(m_dwPageCount, right.m_dwPageCount);
        std::swap(m_dwTotalCount, right.m_dwTotalCount);
    }

    [[nodiscard]] auto empty() const -> bool { return m_candidateList.empty(); }

    /**
     * Close current candidate list
     */
    void Close()
    {
        m_candidateList.clear();
        // Reset the indices too: a stale selection/first-index surviving into
        // the next list would highlight the wrong entry until the next
        // SetSelection/SetFirstIndex arrives.
        m_dwFirstIndex  = 0;
        m_dwSelection   = 0;
        m_dwPageSize    = 0;
        m_dwCurrentPage = 0;
        m_dwPageCount   = 0;
        m_dwTotalCount  = 0;
    }

private:
    CandidateList_t m_candidateList;
    DWORD           m_dwFirstIndex{0}; ///< [internal use] the first index of current candidate list. Be used to determine is page up or page down.
    DWORD           m_dwSelection{0};
    DWORD           m_dwPageSize{0};    ///< Candidates per page, as reported by the IME.
    DWORD           m_dwCurrentPage{0}; ///< 0-based page the IME is currently on.
    DWORD           m_dwPageCount{0};   ///< Total number of candidate pages.
    DWORD           m_dwTotalCount{0};  ///< Total number of candidates across all pages.
};
} // namespace Ime

#endif
