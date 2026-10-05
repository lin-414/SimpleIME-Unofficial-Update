//
// Design-preview stub for JamieMods common/log.h: the imguiex sources only
// emit through logger::*, and the preview tool must not pull spdlog.
// Compile with this directory BEFORE JamieMods/common in the include order.
//
#pragma once

#include <string_view>
#include <utility>

namespace logger
{
template <typename... Args>
constexpr void error(std::string_view, Args &&...) noexcept
{
}
template <typename... Args>
constexpr void info(std::string_view, Args &&...) noexcept
{
}
template <typename... Args>
constexpr void warn(std::string_view, Args &&...) noexcept
{
}
template <typename... Args>
constexpr void debug(std::string_view, Args &&...) noexcept
{
}
template <typename... Args>
constexpr void trace(std::string_view, Args &&...) noexcept
{
}
} // namespace logger
