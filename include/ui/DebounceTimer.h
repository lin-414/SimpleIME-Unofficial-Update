//
// Created by jamie on 2026/1/17.
//

#pragma once

#include <chrono>

namespace Ime
{
class DebounceTimer
{
    using Clock = std::chrono::steady_clock;

public:
    explicit DebounceTimer(std::chrono::milliseconds delay) : m_Delay(delay) {}

    void Poke()
    {
        m_LastPokeTime = Clock::now();
        m_IsWaiting    = true;
    }

    auto Check() -> bool
    {
        if (!m_IsWaiting) return false;

        // Level-triggered: returns true exactly once per poke, after the delay.
        if (Clock::now() - m_LastPokeTime >= m_Delay)
        {
            m_IsWaiting = false;
            return true;
        }
        return false;
    }

    auto IsWaiting() const -> bool { return m_IsWaiting; }

    void Reset() { m_IsWaiting = false; }

private:
    std::chrono::milliseconds m_Delay;
    Clock::time_point         m_LastPokeTime;
    bool                      m_IsWaiting = false;
};
} // namespace Ime
