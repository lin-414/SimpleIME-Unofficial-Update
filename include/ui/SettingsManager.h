//
// Created by jamie on 2026/4/6.
//

#pragma once

#include "Settings.h"
#include "configs/ConfigSerializer.h"
#include "configs/settings_converter.h"
#include "path_utils.h"

#include <filesystem>

namespace Ime
{

namespace SettingsManager
{
static constexpr std::string_view CONFIG_FILE_NAME = "SimpleIME.toml";

auto ConfigFilePath() -> std::filesystem::path
{
    return utils::GetPluginInterfaceDir() / CONFIG_FILE_NAME;
}

constexpr auto Load() -> Settings
{
    const auto configuration = ConfigSerializer::LoadConfiguration(ConfigFilePath());
    return ConvertConfigurationToSettings(configuration);
}

constexpr auto Save(const Settings &settings) -> bool
{
    const auto config = ConvertSettingsToConfiguration(settings);
    return ConfigSerializer::SaveConfiguration(ConfigFilePath(), config);
}

} // namespace SettingsManager

} // namespace Ime