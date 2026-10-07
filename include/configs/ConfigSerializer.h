//
// Created by jamie on 2026/1/14.
//

#pragma once

#include "ui/Settings.h"

#include <filesystem>
#include <string>
#include <vector>

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
    /// Keys present in the file with a value type the schema does not accept;
    /// the loader kept the default for them. Empty unless kind == Ok.
    std::vector<std::string> ignoredKeys;
};

//! @brief Save the configuration to a file. Will overwrite the file if it already exists.
//! All exceptions will be caught and logged, and the function will not throw.
//! Returns false on failure (the user is notified through the ErrorNotifier
//! and the temp file is removed). The temp-file + rename write keeps the
//! original file intact unless the direct-write fallback fails too.
auto SaveConfiguration(const std::filesystem::path &filePath, const Configuration &configuration) -> bool;

auto LoadConfiguration(const std::filesystem::path &filePath) -> Configuration;

//! @brief Strictly parse @p filePath for validation; unlike LoadConfiguration no
//! defaults are silently applied, so the Advanced panel can surface a broken
//! user file instead of it failing invisibly at startup. Never throws.
auto ValidateConfiguration(const std::filesystem::path &filePath) -> ConfigStatus;
} // namespace ConfigSerializer

} // namespace Ime
