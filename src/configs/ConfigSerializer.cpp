//
// Created by jamie on 2026/1/14.
//
#include "configs/ConfigSerializer.h"

#include "configs/configuration.h"
#include "imguiex/ErrorNotifier.h"
#include "log.h"
#include "toml/toml.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <ios>
#include <iosfwd>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace Ime::ConfigSerializer
{
namespace
{

constexpr auto KEY_SHORTCUT = "shortcut";

// Section keys
constexpr auto KEY_SECTION_CORE       = "core";
constexpr auto KEY_SECTION_RESOURCES  = "resources";
constexpr auto KEY_SECTION_APPEARANCE = "appearance";
constexpr auto KEY_SECTION_INPUT      = "input";
constexpr auto KEY_SECTION_LOGGING    = "logging";

// Core keys
constexpr auto KEY_ENABLE_TSF                        = "enable_tsf";
constexpr auto KEY_ENABLE_MOD                        = "enable_mod";
constexpr auto KEY_FIX_INCONSISTENT_TEXT_ENTRY_COUNT = "fix_inconsistent_text_entry_count";
constexpr auto KEY_AUTO_TOGGLE_KEYBOARD              = "auto_toggle_keyboard";
constexpr auto KEY_FORCE_DPI_AWARENESS               = "force_dpi_awareness";

// Logging keys
constexpr auto KEY_LOG_LEVEL       = "level";
constexpr auto KEY_LOG_FLUSH_LEVEL = "flush_level";

// Resources keys
constexpr auto KEY_TRANSLATION_DIR = "translation_dir";
constexpr auto KEY_FONTS           = "fonts";

// Appearance keys
constexpr auto KEY_ZOOM                     = "zoom";
constexpr auto KEY_LANGUAGE                 = "language";
constexpr auto KEY_THEME_STYLE              = "theme_style";
constexpr auto KEY_THEME_SOURCE_COLOR       = "theme_source_color";
constexpr auto KEY_THEME_DARK_MODE          = "theme_dark_mode";
constexpr auto KEY_THEME_CONTRAST_LEVEL     = "theme_contrast_level";
constexpr auto KEY_ERROR_DISPLAY_DURATION   = "error_display_duration";
constexpr auto KEY_VERTICAL_CANDIDATE_LIST  = "vertical_candidate_list";
constexpr auto KEY_AUTO_TOGGLE_LANGUAGE_BAR = "auto_toggle_language_bar";

// Input keys
constexpr auto KEY_ENABLE_UNICODE_PASTE   = "enable_unicode_paste";
constexpr auto KEY_KEEP_IME_OPEN          = "keep_ime_open";
constexpr auto KEY_POS_UPDATE_POLICY      = "pos_update_policy";
constexpr auto KEY_MERIDIAN_SUPPORT       = "meridian_support";
constexpr auto KEY_PRISMA_AVOIDANCE       = "prisma_avoidance";
constexpr auto KEY_SKSEMF_SUPPORT         = "skse_menu_framework_support";
constexpr auto KEY_LAST_NATIVE_CONVERSION = "last_native_conversion";

//! @brief Format the configuration to a TOML string with comments for better readability.
//! May throw `toml::exception` if the configuration contains unsupported types or values.
auto FormatConfigurationToToml(const Configuration &configuration) -> std::string
{
    using Comments = std::vector<std::string>;

    const Comments shortcutComment = {
        " 快捷键配置，格式: <key> 或 <modifier(s)> + <key>，多个部分用 '+' 分隔，忽略空格",
        " <key>      : F1-F12, NumPad0-9, A-Z, 0-9 等 ImGui 支持的按键名",
        " <modifier> : ctrl, shift, alt, super (可组合)",
        R"( 示例: "F2", "ctrl + A", "ctrl + shift + alt + A")",
        " 注意: 不支持多个普通键组合 (如 A + B)，末尾不能为 '+'；单独的修饰键 (如 \"ctrl\") 也是非法的，非法配置将使用默认值",
        "默认值为 F2"
    };
    const Comments enableTsfComment = {
        " 强烈建议开启，只需要保证您的系统版本大于 Window 8.1(除非您手动将 TSF 关闭了，或者您是某些极简系统). 关闭将使用 Imm32, 支持不如 TSF"
    };
    const Comments enableModComment = {" 是否启用 Mod 功能"};
    const Comments logLevelComment  = {R"( "trace", "debug", "info", "warn", "error", "critical", "off")"};
    const Comments zoomComment      = {" UI缩放倍率，有效值为 0.5 - 2.0, 请使用 0.25 的整数倍", " 可以指定为负数，表示应用当前显示器的缩放倍率"};
    const Comments fixInconsistentTextEntryCountComment = {
        " 罕见的修复开关，打开时: 如果鼠标不可见，但依然存在激活的输入框, Mod 会主动禁用 IME 输入. 因为鼠标不可见通常意味着当前处于正常游戏阶段",
        " 但是如果第三方 Menu 存在激活的输入框，但禁用了鼠标(例如某些极简 UI)，此开关可能会错误的禁用 IME，需要关闭此开关来修复这个问题",
    };
    const Comments autoToggleKeyboardComment = {
        " 自动打开/关闭键盘",
        " 在 IME 激活/关闭的同时打开/关闭键盘(切换本地语言/英文输入状态)",
    };
    const Comments forceDpiAwarenessComment = {
        " 进程级 DPI 感知 (默认开启)",
        " Skyrim 本体不声明 DPI 感知，在系统缩放非 100% 的屏幕上，整个游戏画面(包括本 Mod 的设置界面)",
        " 会被系统拉伸渲染，导致字体普遍发虚。开启后 SimpleIME 在游戏创建窗口前将进程声明为 Per-Monitor-V2 感知，",
        " 画面 1:1 渲染，文字恢复锐利，UI 缩放也会跟随显示器真实缩放倍率。",
        " 注意: 开启后窗口模式的游戏窗口不再被放大(在缩放屏幕上看起来变小了)，可将游戏内分辨率调到显示器原生分辨率。",
        " 若因此出现兼容性问题(极少数第三方 Mod 的 UI 坐标异常)，可将此项改为 false 恢复原行为。",
    };
    const Comments meridianSupportComment = {
        " Meridian UI 输入支持",
        " 启用后，当 Meridian 界面(CEF 渲染，如 Tailor 裁缝店)获得焦点时自动激活 IME，",
        " 并将上屏文字直接提交到其网页输入框。未安装 MeridianUI.dll 时此选项无效果。",
    };
    const Comments prismaAvoidanceComment = {
        " Prisma UI 避让模式",
        " Prisma 界面(如 Outfit Wheeler)自带原生输入法处理，此开关打开时 SimpleIME 会在",
        " Prisma 界面持有键盘焦点期间自动让位，避免两套输入法互相抢占。关闭后 SimpleIME",
        " 将无视 Prisma(仅在明确知道后果时才建议关闭)。",
    };
    const Comments skseMenuFrameworkSupportComment = {
        " SKSE Menu Framework 输入支持",
        " 启用后，SKSEMF 界面(ImGui 实现，如使用该框架的设置菜单)的文本框聚焦时自动激活 IME，",
        " 并将上屏中文直接注入输入框。未安装 SKSEMenuFramework.dll(或版本低于 3.7)时此选项无效果。",
    };
    const Comments lastNativeConversionComment = {
        " 运行时缓存，非用户配置：游戏内最后观察到的 中/英 输入状态(true=中文)",
        " 由 Mod 在每次保存配置时自动更新，用于下次启动游戏时语言栏的初始显示。",
        " 微信等输入法不通过系统接口公布中/英状态，没有这个缓存时首次进入输入框无法预判。",
        " 请勿手动修改。",
    };
    const Comments fontPathListComment = {
        " [可选] 用于 SimpleIME 的字体文件路径列表，支持 ttf 和 otf 格式，Mod 会按照列表顺序加载字体并合并到一起",
        " 如果列表为空或所有字体文件都无效，将使用系统默认字体",
        " 示例: [\"C:/Windows/Fonts/simhei.ttf\", \"C:/Windows/Fonts/seguiemj.ttf\"]"
    };

    const toml::table logging{
        {KEY_LOG_LEVEL,       {configuration.logging.level, logLevelComment}},
        {KEY_LOG_FLUSH_LEVEL, configuration.logging.flushLevel              },
    };

    const toml::table core{
        {{KEY_SHORTCUT, {configuration.shortcut, shortcutComment}},
         {KEY_ENABLE_TSF, {configuration.enableTsf, enableTsfComment}},
         {KEY_ENABLE_MOD, {configuration.enableMod, enableModComment}},
         {KEY_FIX_INCONSISTENT_TEXT_ENTRY_COUNT, {configuration.fixInconsistentTextEntryCount, fixInconsistentTextEntryCountComment}},
         {KEY_AUTO_TOGGLE_KEYBOARD, {configuration.autoToggleKeyboard, autoToggleKeyboardComment}},
         {KEY_FORCE_DPI_AWARENESS, {configuration.forceDpiAwareness, forceDpiAwarenessComment}},
         {KEY_SECTION_LOGGING, logging}}
    };
    const toml::table resources{
        {KEY_TRANSLATION_DIR, {configuration.resources.translationDir, {" 翻译文件目录"}}},
        {KEY_FONTS,           {configuration.resources.fontPathList, fontPathListComment}},
    };

    toml::value sourceColor          = {configuration.appearance.themeSourceColor, {" RGB 颜色, 不包含 alpha"}};
    sourceColor.as_integer_fmt().fmt = toml::integer_format::hex;
    const toml::table appearance{
        {KEY_ZOOM,                     {configuration.appearance.zoom, zoomComment}                                                    },
        {KEY_LANGUAGE,                 configuration.appearance.language                                                               },
        {KEY_THEME_STYLE,              {configuration.appearance.themeStyle, {R"( 主题样式，默认 "default" 固定配色; 在主题构建器自定义主题颜色后自动变为 "material")"}}},
        {KEY_THEME_SOURCE_COLOR,       sourceColor                                                                                     },
        {KEY_THEME_DARK_MODE,          configuration.appearance.themeDarkMode                                                          },
        {KEY_THEME_CONTRAST_LEVEL,     {configuration.appearance.themeContrastLevel, {" 对比度，-1.0 - 1.0，默认值为 0.0"}}            },
        {KEY_ERROR_DISPLAY_DURATION,   {configuration.appearance.errorDisplayDuration, {" 错误信息显示持续时间 (秒)，-1 为不自动关闭"}}},
        {KEY_VERTICAL_CANDIDATE_LIST,  {configuration.appearance.verticalCandidateList, {" 垂直显示候选词列表"}}                       },
        {KEY_AUTO_TOGGLE_LANGUAGE_BAR, {configuration.appearance.autoToggleLanguageBar, {" IME 激活/隐藏的同时，自动激活/隐藏语言栏"}} },
    };
    const toml::table input{
        {KEY_ENABLE_UNICODE_PASTE, configuration.input.enableUnicodePaste},
        {KEY_KEEP_IME_OPEN,        configuration.input.keepImeOpen       },
        {KEY_POS_UPDATE_POLICY,    configuration.input.posUpdatePolicy   },
        {KEY_MERIDIAN_SUPPORT,     {configuration.input.meridianSupport, meridianSupportComment}},
        {KEY_PRISMA_AVOIDANCE,     {configuration.input.prismaAvoidance, prismaAvoidanceComment}},
        {KEY_SKSEMF_SUPPORT,       {configuration.input.skseMenuFrameworkSupport, skseMenuFrameworkSupportComment}},
        {KEY_LAST_NATIVE_CONVERSION, {configuration.input.lastNativeConversion, lastNativeConversionComment}},
    };
    const toml::value tomlTable = {
        toml::table{
                    {KEY_SECTION_CORE, core},
                    {KEY_SECTION_RESOURCES, resources},
                    {KEY_SECTION_APPEARANCE, appearance},
                    {KEY_SECTION_INPUT, input},
                    }
    };

    return toml::format(tomlTable);
}

/// Strictly-typed key reader. toml::find_or swallows type mismatches into the
/// default, making a mistyped key indistinguishable from an absent one; here
/// a present-but-wrong value is warned about, collected and ignored.
template <typename T>
auto findAndSetStrict(toml::value &tomlTable, const char *key, T &value, std::vector<std::string> &ignoredKeys) -> void
{
    if (!tomlTable.contains(key))
    {
        return; // absent: the default stays
    }
    auto &node = tomlTable.at(key);
    if constexpr (std::is_same_v<T, bool>)
    {
        if (node.is_boolean())
        {
            value = node.as_boolean();
            return;
        }
    }
    else if constexpr (std::is_integral_v<T>)
    {
        if (node.is_integer())
        {
            value = static_cast<T>(node.as_integer());
            return;
        }
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        // toml11 is strictly typed: a bare `zoom = 1` is an integer, so the
        // floating fields also accept integers.
        if (node.is_floating())
        {
            value = static_cast<T>(node.as_floating());
            return;
        }
        if (node.is_integer())
        {
            value = static_cast<T>(node.as_integer());
            return;
        }
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        if (node.is_string())
        {
            value = node.as_string();
            return;
        }
    }
    else if constexpr (std::is_same_v<T, std::vector<std::string>>)
    {
        if (node.is_array() && std::ranges::all_of(node.as_array(), [](const auto &element) { return element.is_string(); }))
        {
            value = toml::get<std::vector<std::string>>(node);
            return;
        }
    }
    logger::warn("config key {} has unexpected type, ignoring user value", key);
    ignoredKeys.emplace_back(key);
}

auto ParseConfigurationFromToml(toml::value &rawToml, std::vector<std::string> *ignoredKeysOut) -> Configuration
{
    std::vector<std::string> ignoredKeys;
    auto apply = [&ignoredKeys](auto &tomlTable, const char *key, auto &value) -> void {
        findAndSetStrict(tomlTable, key, value, ignoredKeys);
    };
    Configuration config = GetDefaultConfiguration();
    if (rawToml.contains(KEY_SECTION_CORE))
    {
        auto &coreToml = rawToml[KEY_SECTION_CORE];
        apply(coreToml, KEY_SHORTCUT, config.shortcut);
        apply(coreToml, KEY_ENABLE_TSF, config.enableTsf);
        apply(coreToml, KEY_ENABLE_MOD, config.enableMod);
        apply(coreToml, KEY_FIX_INCONSISTENT_TEXT_ENTRY_COUNT, config.fixInconsistentTextEntryCount);
        apply(coreToml, KEY_AUTO_TOGGLE_KEYBOARD, config.autoToggleKeyboard);
        apply(coreToml, KEY_FORCE_DPI_AWARENESS, config.forceDpiAwareness);
        if (coreToml.contains(KEY_SECTION_LOGGING))
        {
            auto &loggingToml = coreToml[KEY_SECTION_LOGGING];
            apply(loggingToml, KEY_LOG_LEVEL, config.logging.level);
            apply(loggingToml, KEY_LOG_FLUSH_LEVEL, config.logging.flushLevel);
        }
    }

    if (rawToml.contains(KEY_SECTION_RESOURCES))
    {
        auto &resourcesToml = rawToml[KEY_SECTION_RESOURCES];
        apply(resourcesToml, KEY_TRANSLATION_DIR, config.resources.translationDir);
        apply(resourcesToml, KEY_FONTS, config.resources.fontPathList);
    }

    if (rawToml.contains(KEY_SECTION_APPEARANCE))
    {
        auto &appearanceToml = rawToml[KEY_SECTION_APPEARANCE];
        apply(appearanceToml, KEY_ZOOM, config.appearance.zoom);
        apply(appearanceToml, KEY_LANGUAGE, config.appearance.language);
        apply(appearanceToml, KEY_THEME_STYLE, config.appearance.themeStyle);
        apply(appearanceToml, KEY_THEME_SOURCE_COLOR, config.appearance.themeSourceColor);
        apply(appearanceToml, KEY_THEME_DARK_MODE, config.appearance.themeDarkMode);
        apply(appearanceToml, KEY_THEME_CONTRAST_LEVEL, config.appearance.themeContrastLevel);
        apply(appearanceToml, KEY_ERROR_DISPLAY_DURATION, config.appearance.errorDisplayDuration);
        apply(appearanceToml, KEY_VERTICAL_CANDIDATE_LIST, config.appearance.verticalCandidateList);
        apply(appearanceToml, KEY_AUTO_TOGGLE_LANGUAGE_BAR, config.appearance.autoToggleLanguageBar);
    }

    if (rawToml.contains(KEY_SECTION_INPUT))
    {
        auto &input = rawToml[KEY_SECTION_INPUT];
        apply(input, KEY_ENABLE_UNICODE_PASTE, config.input.enableUnicodePaste);
        apply(input, KEY_KEEP_IME_OPEN, config.input.keepImeOpen);
        apply(input, KEY_POS_UPDATE_POLICY, config.input.posUpdatePolicy);
        apply(input, KEY_MERIDIAN_SUPPORT, config.input.meridianSupport);
        apply(input, KEY_PRISMA_AVOIDANCE, config.input.prismaAvoidance);
        apply(input, KEY_SKSEMF_SUPPORT, config.input.skseMenuFrameworkSupport);
        apply(input, KEY_LAST_NATIVE_CONVERSION, config.input.lastNativeConversion);
    }
    if (ignoredKeysOut != nullptr)
    {
        *ignoredKeysOut = std::move(ignoredKeys);
    }
    return config;
}
} // namespace

auto SaveConfiguration(const std::filesystem::path &filePath, const Configuration &configuration) -> bool
{
    // Write to a temp file first, then replace atomically: opening the real
    // file with trunc used to mean a mid-write failure left a truncated
    // (config-losing) file behind. MSVC's filesystem::rename implements
    // POSIX semantics (replaces the target).
    std::filesystem::path tempPath = filePath;
    tempPath += ".tmp";
    bool success = false;
    try
    {
        const auto tomlString = FormatConfigurationToToml(configuration);

        {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(tempPath, std::ios::out | std::ios::trunc);
            file << tomlString;
        }
        std::error_code renameError;
        std::filesystem::rename(tempPath, filePath, renameError);
        if (renameError)
        {
            logger::warn("Atomic config replace failed ({}), falling back to a direct write.", renameError.message());
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(filePath, std::ios::out | std::ios::trunc);
            file << tomlString;
        }
        logger::info("Configuration saved successfully to {}", filePath.generic_string());
        success = true;
    }
    catch (const std::ios_base::failure &e)
    {
        logger::error("File IO error while saving configuration: {}", e.what());
    }
    catch (toml::exception &exception)
    {
        logger::error("Parse configuration failed! Can't save configuration: {}", exception.what());
    }
    catch (const std::exception &e)
    {
        logger::error("Unknown error during serialization: {}", e.what());
    }
    if (!success)
    {
        // Best effort: a failed rename leaves the temp file behind, and a
        // failed write leaves a partial one.
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        ErrorNotifier::GetInstance().Error("Failed to save the configuration");
    }
    return success;
}

auto LoadConfiguration(const std::filesystem::path &filePath) -> Configuration
{
    try
    {
        auto tomlValue = toml::parse(filePath);
        return ParseConfigurationFromToml(tomlValue, nullptr);
    }
    catch (toml::exception &exception)
    {
        logger::error("Parse configuration failed! Fallback to default configuration: {}", exception.what());
    }
    return GetDefaultConfiguration();
}

auto ValidateConfiguration(const std::filesystem::path &filePath) -> ConfigStatus
{
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec))
    {
        return {.kind = ConfigStatusKind::NotFound, .detail = {}};
    }
    try
    {
        auto                     tomlValue = toml::parse(filePath);
        std::vector<std::string> ignoredKeys;
        (void)ParseConfigurationFromToml(tomlValue, &ignoredKeys);
        return {.kind = ConfigStatusKind::Ok, .detail = {}, .ignoredKeys = std::move(ignoredKeys)};
    }
    catch (toml::exception &exception)
    {
        return {.kind = ConfigStatusKind::ParseError, .detail = exception.what()};
    }
    catch (const std::exception &e)
    {
        return {.kind = ConfigStatusKind::ParseError, .detail = e.what()};
    }
}

} // namespace Ime::ConfigSerializer
