#pragma once

#include <atomic>

namespace Hooks
{
/// RAII latch over a bool flag, in the two shapes the bridges need.
///
/// The `std::atomic<bool>` form is a single-flight guard: the constructor
/// exchanges in true and `owned()` reports the win; the destructor releases
/// only what it acquired, so every early return (and an exception) clears the
/// flag — no manual store at each exit.
///
/// The plain `bool` form is a re-entrancy suppressor for single-threaded
/// contexts: it sets the flag for the scope and clears it on exit.
template <typename Flag>
class ScopeFlag;

template <>
class ScopeFlag<std::atomic<bool>>
{
public:
    explicit ScopeFlag(std::atomic<bool> &flag)
        : m_flag(flag), m_owned(!flag.exchange(true, std::memory_order_acq_rel))
    {}
    ~ScopeFlag()
    {
        if (m_owned)
        {
            m_flag.store(false, std::memory_order_release);
        }
    }
    ScopeFlag(const ScopeFlag &)            = delete;
    ScopeFlag &operator=(const ScopeFlag &) = delete;
    [[nodiscard]] bool owned() const { return m_owned; }

private:
    std::atomic<bool> &m_flag;
    bool               m_owned;
};

template <>
class ScopeFlag<bool>
{
public:
    explicit ScopeFlag(bool &flag) : m_flag(flag) { flag = true; }
    ~ScopeFlag() { m_flag = false; }
    ScopeFlag(const ScopeFlag &)            = delete;
    ScopeFlag &operator=(const ScopeFlag &) = delete;

private:
    bool &m_flag;
};

// The primary template is incomplete (only the specializations exist), so the
// implicit guides never materialize — deduce from the bound flag directly.
template <typename Flag>
ScopeFlag(Flag &) -> ScopeFlag<Flag>;
} // namespace Hooks
