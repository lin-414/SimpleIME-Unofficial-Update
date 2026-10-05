//
// Created by jamie on 2026/1/14.
//

#pragma once

#include "ui/Settings.h"

#include <filesystem>
#include <string>

namespace Ime
{

struct Configuration;

namespace ConfigSerializer
{
//! How a validation attempt against the on-disk configuration file ended.
enum class ConfigStatusKind
{
    Ok,         ///< the file parses cleanly
    NotFound,   ///< the file does not exist (the plugin runs on defaults)
    ParseError, ///< the file exists but TOML parsing rejected it
};

struct ConfigStatus
{
    ConfigStatusKind kind    = ConfigStatusKind::Ok;
    std::string      detail; ///< raw error message; empty when kind == Ok
};

//! @brief Save the configuration to a file. Will overwrite the file if it already exists.
//! All exceptions will be caught and logged, and the function will not throw.
//! The file shall not be written if any error occurs during serialization, and the original file will be kept intact.
void SaveConfiguration(const std::filesystem::path &filePath, const Configuration &configuration);

auto LoadConfiguration(const std::filesystem::path &filePath) -> Configuration;

//! @brief Strictly parse @p filePath for validation; unlike LoadConfiguration no
//! defaults are silently applied, so the Advanced panel can surface a broken
//! user file instead of it failing invisibly at startup. Never throws.
auto ValidateConfiguration(const std::filesystem::path &filePath) -> ConfigStatus;
} // namespace ConfigSerializer

} // namespace Ime
