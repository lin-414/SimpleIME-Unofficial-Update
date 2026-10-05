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

    [[nodiscard]] auto FirstIndex() const -> DWORD { return m_dwFirstIndex; }

    [[nodiscard]] auto Selection() const -> DWORD { return m_dwSelection; }

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
        m_dwFirstIndex = 0;
        m_dwSelection  = 0;
    }

private:
    CandidateList_t m_candidateList;
    DWORD           m_dwFirstIndex{0}; ///< [internal use] the first index of current candidate list. Be used to determine is page up or page down.
    DWORD           m_dwSelection{0};
};
} // namespace Ime

#endif
