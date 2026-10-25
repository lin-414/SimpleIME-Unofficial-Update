#pragma once

#include <atomic>

namespace Ime
{

//! A copyable stand-in for std::atomic<bool> usable inside plain aggregate
//! Settings structs. The aggregate stays an aggregate (designated initializers
//! and the implicit memberwise copy keep working), while cross-thread
//! read/write of the field is well-defined. A copy takes a snapshot of the
//! current value; the cells are not shared.
struct AtomicBool
{
    std::atomic_bool value{ false };

    AtomicBool() = default;
    AtomicBool(const AtomicBool &other) noexcept : value(other.value.load()) {}
    AtomicBool &operator=(const AtomicBool &other) noexcept
    {
        if (this != &other)
        {
            value.store(other.value.load());
        }
        return *this;
    }

    AtomicBool(bool desired) noexcept : value(desired) {}
    AtomicBool &operator=(bool desired) noexcept
    {
        value.store(desired);
        return *this;
    }

    operator bool() const noexcept { return value.load(); }

    void store(bool desired) noexcept { value.store(desired); }
    [[nodiscard]] bool load() const noexcept { return value.load(); }
};

} // namespace Ime
