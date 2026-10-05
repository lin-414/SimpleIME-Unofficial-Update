//
// Design-preview mock of the redesigned SimpleIME ToolWindow.
//
// This is a faithful port of the draw code in src/ui/ToolWindow.cpp and
// src/ui/panels/AppearancePanel.cpp with the game dependencies stubbed out
// (Skyrim menu API, ImeController, Core::State, the i18n translator). The
// M3 component calls — the parts that determine layout — are identical, so
// findings from this preview transfer to the in-game window.
//

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <span>
#include <vector>

#include "imgui.h"
#include "icons.h"
#include "imguiex/imguiex_m3.h"
#include "imguiex/m3/facade/button_groups.h"
#include "imguiex/m3/spec/appbar.h"
#include "imguiex/m3/spec/checkbox.h"
#include "imguiex/m3/spec/icon_button.h"
#include "imguiex/m3/spec/layout.h"
#include "imguiex/m3/spec/others.h"
#include "imguiex/m3/spec/text_field.h"
#include "ui/panels/PanelWidgets.h"

namespace M3Spec = ImGuiEx::M3::Spec;
namespace UI      = Ime::UI;
using ColorRole   = M3Spec::ColorRole;
using namespace Ime; // icon glyph constants

// ImGui 1.92 dynamic fonts make glyph ranges obsolete; the preview loads its
// fonts directly, so the imgui_manager.cpp AddFont is provided in the main TU.

namespace mock
{
namespace
{
//! zh UI strings (Translate() stands in for the loaded translator).
struct StrEntry
{
    std::string_view key;
    std::string_view zh;
};
constexpr std::array<StrEntry, 114> STRINGS{{
    {"Settings", "设置"},
    {"Close", "关闭"},
    {"Cancel", "取消"},
    {"Sidebar.InputStatus", "输入与状态"},
    {"Sidebar.Display", "显示与主题"},
    {"Sidebar.FontBuilder", "字体"},
    {"Sidebar.Advanced", "兼容与高级"},
    {"SaveHint", "更改立即生效，关闭设置时自动保存。"},
    {"Page.InputStatus.Support", "当前状态、日常输入、呼出快捷键。"},
    {"Page.Display.Support", "预览候选布局，并调整界面外观、主题与显示行为。"},
    {"Page.Fonts.Support", "按“搜索、预览、添加、设为默认”的顺序构建字体。"},
    {"Page.Advanced.Support", "兼容性排查、诊断信息与日志；其余高级配置仍通过 TOML 管理。"},
    {"Advanced.Troubleshooting", "兼容性排查"},
    {"Advanced.ConfigTitle", "配置文件中的其他选项"},
    {"Advanced.ConfigPath", "配置文件：interface/SimpleIME/SimpleIME.toml"},
    {"Advanced.ConfigDescription", "TSF、Meridian／Prisma／SKSE Menu Framework 兼容开关和日志级别已可在设置界面调整；翻译目录等其余选项仍在配置文件中管理，部分项目需重启游戏后生效。"},
    {"Appearance.General", "常规"},
    {"Appearance.CandidateWindow", "候选窗口"},
    {"Appearance.CandidatePreview", "候选窗口预览"},
    {"Appearance.ZoomFollowMonitor", "跟随显示器缩放"},
    {"Behaviour.Policy.Cursor", "基于鼠标位置"},
    {"Behaviour.Policy.Caret", "基于插入符位置"},
    {"Behaviour.Policy.None", "无"},
    {"Appearance.Languages", "语言"},
    {"Appearance.Zoom", "缩放"},
    {"Appearance.ThemeMode", "主题模式"},
    {"Appearance.Light", "亮色"},
    {"Appearance.Dark", "暗色"},
    {"Appearance.Theme", "主题"},
    {"Appearance.Customize", "自定义"},
    {"Appearance.ResetTheme", "重置主题"},
    {"Appearance.VerticalCandidateList", "垂直显示候选列表"},
    {"Appearance.AutoToggleLanguageBar", "自动显示/隐藏语言栏"},
    {"Behaviour.LiveStatus", "实时状态"},
    {"Behaviour.StatusMod", "模组"},
    {"Behaviour.StatusIme", "输入法"},
    {"Behaviour.StatusFocus", "键盘焦点"},
    {"Behaviour.StatusModOn", "模组已启用"},
    {"Behaviour.StatusModOff", "模组已停用"},
    {"Behaviour.StatusImeOn", "IME 已开启"},
    {"Behaviour.StatusImeOff", "IME 已关闭"},
    {"Behaviour.StateOn", "开启"},
    {"Behaviour.StateOff", "关闭"},
    {"Behaviour.Input", "日常输入"},
    {"Behaviour.EnableMod", "启用模组"},
    {"Behaviour.EnableModToolTip", "关闭后相关输入设置暂不可操作。"},
    {"Behaviour.FixInconsistent", "修复不一致的文本输入框数量"},
    {"Behaviour.FixInconsistentToolTip", "如果选中，mod 将尝试在检测到不一致的文本输入框数量时禁用 IME。如果存在一个第三方禁用了光标但存在活动文本输入框，可能会导致一些问题。"},
    {"Behaviour.AutoToggleKeyboard", "自动打开/关闭键盘"},
    {"Behaviour.AutoToggleKeyboardToolTip", "IME 激活时，自动打开/关闭键盘，这意味着切换 IME 的输入模式(中/英)"},
    {"Behaviour.ShortAutoToggleKeyboard", "自动切换键盘"},
    {"Behaviour.ShortUnicodePaste", "Unicode 粘贴"},
    {"Behaviour.EnableUnicodePaste", "启用 Unicode 粘贴"},
    {"Behaviour.EnableUnicodePasteToolTip", "启用或关闭 Mod 提供的 unicode 粘贴功能"},
    {"Behaviour.KeepImeOpen", "保持 IME 开启"},
    {"Behaviour.KeepImeOpenTooltip", "主要用于在一些不支持 IME 的输入框中临时启用 IME"},
    {"Behaviour.Shortcut", "呼出快捷键"},
    {"Behaviour.ChangeShortcut", "更改"},
    {"Behaviour.ResetShortcut", "重置"},
    {"Behaviour.Policy", "IME 窗口位置更新策略"},
    {"Advanced.LogFile", "日志文件"},
    {"Advanced.CopyLogPath", "复制日志路径"},
    {"Advanced.Copied", "已复制"},
    {"Advanced.ConfigStatus", "配置读取状态"},
    {"Advanced.ConfigStatusOk", "正常"},
    {"Advanced.ConfigStatusError", "解析失败"},
    {"Advanced.ConfigStatusMissing", "文件不存在"},
    {"Copy", "复制"},
    {"Open", "打开"},
    {"Advanced.Diagnostics", "诊断信息"},
    {"Advanced.DiagnosticsSupport", "复制版本、运行环境与桥接状态摘要，反馈问题时可直接附上。"},
    {"Advanced.Logging", "日志与错误提示"},
    {"Advanced.LogLevel", "日志级别"},
    {"Advanced.LogLevelSupport", "更详细的日志便于排查问题，修改立即生效。"},
    {"Advanced.ErrorDuration", "错误提示时长"},
    {"Advanced.ErrorDurationSupport", "错误信息显示的持续时间，超过后自动关闭。"},
    {"Advanced.ErrorDurationNever", "不自动关闭"},
    {"Advanced.Environment", "运行环境"},
    {"Advanced.DpiAwareness", "进程 DPI 感知"},
    {"Advanced.DpiAwarenessSupport", "在插件加载时读取，修改需重启游戏。开启后画面在缩放屏幕上 1:1 渲染，文字更锐利。"},
    {"Advanced.TsfFramework", "输入法框架"},
    {"Advanced.TsfFrameworkSupport", "在插件加载时读取。TSF 是现代输入法框架，兼容性更好；仅老旧或精简系统需要 Imm32。"},
    {"Advanced.StateEnabled", "已启用"},
    {"Advanced.StateDisabled", "未启用"},
    {"Behaviour.Compatibility", "兼容性"},
    {"Behaviour.MeridianSupport", "Meridian UI 输入支持"},
    {"Behaviour.MeridianSupportToolTip", "当 Meridian 界面（CEF 渲染，如裁缝店）获得焦点时自动激活 IME，并将上屏文字直接提交到其网页输入框。未安装 MeridianUI.dll 时此选项无效果。修改后需重启游戏生效。"},
    {"Behaviour.PrismaAvoidance", "Prisma UI 避让模式"},
    {"Behaviour.PrismaAvoidanceToolTip", "Prisma 界面（如 Outfit Wheeler）自带原生输入法处理，开启后 SimpleIME 会在其持有键盘焦点期间自动让位，避免两套输入法互相抢占。修改后需重启游戏生效。"},
    {"Behaviour.SkseMenuFrameworkSupport", "SKSE Menu Framework 输入支持"},
    {"Behaviour.SkseMenuFrameworkSupportToolTip", "SKSEMF 界面（ImGui 实现，如使用该框架的设置菜单）的文本框聚焦时自动激活 IME，并将上屏中文直接注入输入框。需要 SKSEMenuFramework.dll 3.7 及以上版本。修改后需重启游戏生效。"},
    {"Compat.Active", "已生效"},
    {"Compat.Off", "已停用"},
    {"Compat.Pending", "等待初始化"},
    {"Compat.NotDetected", "未检测到"},
    {"Compat.Standoff", "存在冲突"},
    {"Compat.Unavailable", "不可用"},
    {"Compat.RestartEnable", "重启后生效"},
    {"Compat.RestartDisable", "重启后停用"},
    {"FontBuilder.Search", "搜索字体"},
    {"FontBuilder.BuildQueue", "构建队列"},
    {"FontBuilder.PreviewBuild", "预览构建结果"},
    {"FontBuilder.BuildQueueEmpty", "预览字体后，点击右侧的 + 将其加入构建队列。"},
    {"FontBuilder.SetAsDefault", "设置为默认字体"},
    {"FontBuilder.ResetBuilder", "清空构建"},
    {"FontBuilder.Warning", "警告"},
    {"FontBuilder.Help", "帮助"},
    {"FontBuilder.Add", "添加字体"},
    {"FontBuilder.NotSelected", "未选字体 (预览默认)"},
    {"FontBuilder.PreviewingPath", "C:\\Windows\\Fonts\\msyh.ttc"},
}};

auto T(const std::string_view key) -> std::string_view
{
    std::string_view text = key;
    for (const auto &entry : STRINGS)
    {
        if (entry.key == key)
        {
            text = entry.zh;
            break;
        }
    }
    // Locale stress: double every resolved string to approximate the longest
    // translation (DE/RU) and flush out fixed-geometry overflows without
    // launching the game. Cached: the returned view must stay stable.
    if (std::getenv("PREVIEW_LANG_STRESS") != nullptr)
    {
        static std::unordered_map<std::string, std::string> stressCache;
        auto &stressed = stressCache[std::string(key)];
        if (stressed.empty())
        {
            stressed = std::string(text) + " " + std::string(text);
        }
        return stressed;
    }
    return text;
}

//! Visits the keycap labels of a chord in display order — modifiers first,
//! then the main key — so the draw path stays declarative.
template <typename Fn>
void ForEachShortcutKeycap(const ImGuiKeyChord chord, Fn &&fn)
{
    if ((chord & ImGuiMod_Ctrl) != 0) fn(std::string_view{"Ctrl"});
    if ((chord & ImGuiMod_Shift) != 0) fn(std::string_view{"Shift"});
    if ((chord & ImGuiMod_Alt) != 0) fn(std::string_view{"Alt"});
    if ((chord & ImGuiMod_Super) != 0) fn(std::string_view{"Super"});
    if (const auto mainKey = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_); mainKey != ImGuiKey_None)
    {
        if (const char *keyName = ImGui::GetKeyName(mainKey); keyName != nullptr)
        {
            fn(std::string_view{keyName});
        }
    }
}

//! The bound chord rendered as physical keycaps (port of ToolWindow.cpp's helper).
//! Positioned entirely via SetCursorScreenPos: SameLine() snaps both X and Y
//! to CursorPosPrevLine (stale after SetCursorScreenPos), which floats the
//! "+ key" part a line above the first keycap instead of centering the group.
void DrawShortcutKeycaps(const ImGuiKeyChord chord)
{
    bool       first    = true;
    const auto spacing  = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::dp<8>());
    const auto baseY    = ImGui::GetCursorScreenPos().y;
    ForEachShortcutKeycap(chord, [&](const std::string_view label) {
        if (!first)
        {
            ImGui::SetCursorScreenPos({ImGui::GetItemRectMax().x + spacing, baseY});
            ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::LabelMedium>("+", ColorRole::onSurfaceVariant);
            ImGui::SetCursorScreenPos({ImGui::GetItemRectMax().x + spacing, baseY});
        }
        UI::Panels::Keycap(label);
        first = false;
    });
}

//! Readable measure for row supporting copy (port of ToolWindow.cpp's helper).
auto SupportingMeasure() -> float
{
    return ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::dp<500>());
}

//! Port of ToolWindow.cpp's BridgeStatusCaption: caption + tone for a
//! compatibility toggle from the bridge's install-time outcome (0 pending,
//! 1 off, 2 standoff, 3 not detected, 4 failed, 5 active — the
//! Hooks::SupportState order) and the switch's current value.
auto BridgeCaption(const bool configOn, const int state) -> std::pair<std::string_view, ColorRole>
{
    switch (state)
    {
        case 5:
            return configOn ? std::pair{T("Compat.Active"), ColorRole::primary}
                            : std::pair{T("Compat.RestartDisable"), ColorRole::onSurfaceVariant};
        case 0:
            return {T("Compat.Pending"), ColorRole::onSurfaceVariant};
        case 1:
            return {configOn ? T("Compat.RestartEnable") : T("Compat.Off"), ColorRole::onSurfaceVariant};
        case 3:
            return {T("Compat.NotDetected"), ColorRole::onSurfaceVariant};
        case 2:
            return {T("Compat.Standoff"), ColorRole::error};
        default:
            return {T("Compat.Unavailable"), ColorRole::error};
    }
}
} // namespace

struct MiniSettings
{
    bool          enableMod                = true;
    bool          fixInconsistentTextEntry = true;
    bool          autoToggleKeyboard       = false;
    bool          enableUnicodePaste       = true;
    bool          keepImeOpen              = false;
    bool          verticalCandidateList    = false;
    bool          autoToggleLanguageBar    = true;
    bool          meridianSupport          = true;
    bool          prismaAvoidance          = true;
    bool          skseMenuFrameworkSupport = true;
    bool          forceDpiAwareness        = true;
    bool          enableTsf                = true;
    int           posUpdatePolicy          = 2; ///< 0 none / 1 cursor / 2 caret
    ImGuiKeyChord shortcut                = ImGuiKey_F2;
    int           logLevel                 = 2; ///< 0 trace .. 6 off (spdlog order)
    int           errorDuration            = 10; ///< seconds; -1 = never auto-close
};

struct MiniState
{
    bool imeOn = true;
    bool focus = false;
    int  meridianBridge = 5; ///< Hooks::SupportState order: 0 pending .. 5 active
    int  prismaBridge   = 3; ///< not detected
    int  sksemfBridge   = 5;
};

extern MiniSettings g_settings;
extern MiniState    g_state;
extern float        g_scrollTo; // --scroll=N: scroll the panel child once for screenshots
extern float        g_windowWidth; // --width=N: first-use window width override (0 = default)

enum class Menu : int
{
    InputStatus,
    Display,
    FontBuilder,
    Advanced,
};

class MockToolWindow
{
public:
    void Draw(MiniSettings &settings, const MiniState &state);

    //! Overridable start state so the preview can be launched straight into a
    //! given tab for screenshots.
    Menu m_menu = Menu::InputStatus;

private:
    void DrawSettingsHeader(MiniSettings &settings);
    void DrawSidebar();
    void DrawAppearance(MiniSettings &settings);
    void DrawFontBuilderPlaceholder();
    void DrawMockFontPane(const ImVec2 &size);
    void DrawMockFontResultCard(const ImVec2 &size);
    void DrawInputStatus(MiniSettings &settings, const MiniState &state);
    void DrawAdvanced(MiniSettings &settings);
    void DrawStatusCard(MiniSettings &settings, const MiniState &state);
    void DrawShortcutSection(MiniSettings &settings);
    void DrawPositionPolicy(MiniSettings &settings);
    void DrawZoomRow();
    void DrawLanguagesRow();
    void DrawThemeModeRow();
    void DrawThemeRow();
    void DrawCandidatePreview(bool vertical);

    bool m_capturing = false;
    float m_copiedAt = -1.0F;
    float m_diagCopiedAt = -1.0F;
    ImGuiTextFilter m_fontFilter{};
    int m_fontSelected = 0;
};

void MockToolWindow::DrawSettingsHeader(MiniSettings &settings)
{
    auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float headerH  = m3Styles.GetPixels(M3Spec::SmallAppbar::ContainerHeight);
    const float padX     = m3Styles.GetPixels(M3Spec::dp<20>());
    const auto  origin   = ImGui::GetCursorScreenPos();
    const float availW   = ImGui::GetContentRegionAvail().x;

    {
        const auto  fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::TitleLarge>();
        const auto &typeScale = m3Styles.GetLastText().currText;
        const std::string title = std::string("SimpleIME / ") + std::string(T("Settings"));
        ImGui::SetCursorScreenPos({origin.x + padX, origin.y + (headerH - typeScale.lineHeight) * 0.5F});
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::TitleLarge>(title, ColorRole::onSurface);
    }

    // Mirrors ToolWindow.cpp: quiet X icon button, label only in the tooltip.
    const auto  closeSizing = M3Spec::GetIconButtonSizing(M3Spec::SizeTips::SMALL, M3Spec::IconButtonWidths::Default);
    const float closeExtent =
        std::max(m3Styles.GetPixels(closeSizing.containerHeight), m3Styles.GetPixels(M3Spec::IconButtonCommon::MinLayoutSize));
    ImGui::SetCursorScreenPos({origin.x + availW - padX - closeExtent, origin.y + (headerH - closeExtent) * 0.5F});
    (void)ImGuiEx::M3::SmallIconButton(ICON_X, ImGuiEx::M3::Spec::IconButtonColors::Standard); // stub: the real window hides the tool window
    ImGuiEx::M3::SetItemToolTip(T("Close"));

    ImGui::SetCursorScreenPos({origin.x, origin.y + headerH});
    ImGuiEx::M3::Divider();
}

void MockToolWindow::DrawSidebar()
{
    auto      &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float width   = m3Styles.GetPixels(M3Spec::dp<200>());
    const float pad     = m3Styles.GetPixels(M3Spec::dp<8>());
    const auto  guard   = ImGuiEx::StyleGuard()
                            .Color<ImGuiCol_ChildBg>(m3Styles.Colors()[ColorRole::surfaceContainerLowest])
                            .Color<ImGuiCol_Border>(m3Styles.Colors()[ColorRole::outlineVariant])
                            .Style<ImGuiStyleVar_WindowPadding>(ImVec2(pad, pad))
                            .Style<ImGuiStyleVar_ItemSpacing>(ImVec2(0.0F, m3Styles.GetPixels(M3Spec::dp<2>())))
                            .Style<ImGuiStyleVar_ChildBorderSize>(1.0F);
    if (!ImGui::BeginChild("##SettingsSidebar", {width, 0.0F}, ImGuiEx::ChildFlags().Borders()))
    {
        ImGui::EndChild();
        return;
    }

    const auto selectMenu = [this](const Menu menu) { m_menu = menu; };

    const float itemH = m3Styles.GetPixels(M3Spec::dp<40>());
    const auto  item  = [&](const char *strId, const std::string_view label, const Menu menu) {
        const bool  selected = m_menu == menu;
        const auto  pos      = ImGui::GetCursorScreenPos();
        const float w        = ImGui::GetContentRegionAvail().x;
        const ImRect bb(pos, {pos.x + w, pos.y + itemH});
        const ImGuiID id = ImGui::GetID(strId);
        ImGui::ItemSize({w, itemH});
        if (!ImGui::ItemAdd(bb, id))
        {
            return;
        }
        bool        hovered = false;
        bool        held    = false;
        const bool  pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        ImGui::RenderNavCursor(bb, id);

        auto *drawList = ImGui::GetWindowDrawList();
        if (selected)
        {
            drawList->AddRectFilled(bb.Min, bb.Max, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::secondaryContainer]), m3Styles.GetPixels(M3Spec::ShapeCorner::Medium));
        }
        else if (hovered || held)
        {
            const auto wash = held ? m3Styles.Colors().Pressed(ColorRole::surfaceContainerHighest, ColorRole::onSurface)
                                   : m3Styles.Colors().Hovered(ColorRole::surfaceContainerHighest, ColorRole::onSurface);
            drawList->AddRectFilled(bb.Min, bb.Max, ImGui::ColorConvertFloat4ToU32(wash), m3Styles.GetPixels(M3Spec::ShapeCorner::Medium));
        }

        float textX = bb.Min.x + m3Styles.GetPixels(M3Spec::dp<12>());
        if (selected)
        {
            const float square  = m3Styles.GetPixels(M3Spec::dp<10>());
            const float centerY = bb.GetCenter().y;
            const float squareY = centerY - square * 0.5F;
            drawList->AddRectFilled(
                {bb.Min.x + m3Styles.GetPixels(M3Spec::dp<12>()), squareY},
                {bb.Min.x + m3Styles.GetPixels(M3Spec::dp<12>()) + square, squareY + square},
                ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::primary]),
                m3Styles.GetPixels(M3Spec::dp<4>())
            );
            textX += square + m3Styles.GetPixels(M3Spec::dp<8>());
        }

        {
            const auto  fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
            const auto  color     = selected ? ColorRole::onSecondaryContainer : (hovered ? ColorRole::onSurface : ColorRole::onSurfaceVariant);
            // The rail is a fixed-width surface: long translations elide with
            // an ellipsis instead of clipping mid-glyph at the border, and the
            // tooltip discloses the full label (port of ToolWindow.cpp).
            const float maxTextW = bb.Max.x - textX - m3Styles.GetPixels(M3Spec::dp<12>());
            const auto  elided   = UI::Panels::ElideText(label, maxTextW);
            const auto  shown    = elided.empty() ? label : std::string_view{elided};
            drawList->AddText(
                ImGui::GetFont(),
                ImGui::GetFontSize(),
                {textX, bb.Min.y + ImGuiEx::M3::CenteredTextOffsetY(itemH)},
                ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[color]),
                shown.data(),
                shown.data() + shown.size()
            );
            if (!elided.empty())
            {
                ImGuiEx::M3::SetItemToolTip(label);
            }
        }

        if (pressed)
        {
            selectMenu(menu);
        }
    };

    item("##NavInputStatus", T("Sidebar.InputStatus"), Menu::InputStatus);
    item("##NavDisplay", T("Sidebar.Display"), Menu::Display);
    item("##NavFontBuilder", T("Sidebar.FontBuilder"), Menu::FontBuilder);
    item("##NavAdvanced", T("Sidebar.Advanced"), Menu::Advanced);

    // The save hint lives in the desktop layout too, pinned to the bottom of
    // the nav card (port of ToolWindow.cpp's sidebar footer).
    {
        const std::string_view hint  = T("SaveHint");
        const float            wrapW = width - pad * 2.0F;
        float                  hintH = 0.0F;
        {
            const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::BodySmall>();
            hintH = ImGui::CalcTextSize(hint.data(), hint.data() + hint.size(), false, wrapW).y;
        }
        const float hintY = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - pad - hintH;
        if (hintY > ImGui::GetCursorScreenPos().y + m3Styles.GetPixels(M3Spec::dp<12>()))
        {
            ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x + pad, hintY});
            ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::BodySmall>(hint, ColorRole::onSurfaceVariant, wrapW);
        }
    }

    ImGui::EndChild();
}

void MockToolWindow::Draw(MiniSettings &settings, const MiniState &state)
{
    constexpr ImVec2 CENTER_ALIGN_PIVOT(0.5F, 0.5F);
    constexpr float  DEFAULT_WINDOW_HEIGHT_FACTOR = 0.88F;

    const auto &viewport  = ImGui::GetMainViewport();
    auto       &m3Styles  = ImGuiEx::M3::Context::GetM3Styles();
    const float margin    = m3Styles.GetPixels(M3Spec::Layout::Compact::Margin);
    const float maxWidth  = std::max(1.0F, std::min(m3Styles.GetPixels(M3Spec::Layout::Large::Breakpoint), viewport->Size.x - margin * 2.0F));
    const float minWidth  = std::min(m3Styles.GetPixels(M3Spec::dp<508>()), maxWidth);
    const float maxHeight = std::max(1.0F, viewport->Size.y - margin * 2.0F);
    const float minHeight = std::min(m3Styles.GetPixels(M3Spec::dp<360>()), maxHeight);
    const float defaultWindowWidth = g_windowWidth > 0.0F ? std::clamp(g_windowWidth, minWidth, maxWidth)
                                                          : std::clamp(m3Styles.GetPixels(1024.0F), minWidth, maxWidth);
    const float defaultWindowHeight = std::clamp(viewport->Size.y * DEFAULT_WINDOW_HEIGHT_FACTOR, minHeight, maxHeight);

    if (ImGui::GetFrameCount() == 30)
    {
        if (FILE *dbg = nullptr; fopen_s(&dbg, "preview_debug.txt", "w") == 0 && dbg != nullptr)
        {
            std::fprintf(
                dbg,
                "[preview] viewport=%.0fx%.0f scale1024=%.0f maxW=%.0f default=%.0fx%.0f\n",
                viewport->Size.x,
                viewport->Size.y,
                m3Styles.GetPixels(1024.0F),
                maxWidth,
                defaultWindowWidth,
                defaultWindowHeight
            );
            std::fclose(dbg);
        }
    }

    ImGui::SetNextWindowSize({defaultWindowWidth, defaultWindowHeight}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, CENTER_ALIGN_PIVOT);
    ImGui::SetNextWindowSizeConstraints({minWidth, minHeight}, {maxWidth, maxHeight});

    if (ImGui::Begin("ToolWindow", nullptr, ImGuiEx::WindowFlags().NoTitleBar()))
    {
        DrawSettingsHeader(settings);

        const float navBreakpoint     = m3Styles.GetPixels(M3Spec::Layout::Expanded::Breakpoint);
        const bool  compactNavigation = ImGui::GetContentRegionAvail().x < navBreakpoint;

        const float bodyMargin = m3Styles.GetPixels(M3Spec::dp<16>());
        const auto  bodyPad   = ImGuiEx::StyleGuard().Style<ImGuiStyleVar_WindowPadding>(ImVec2(bodyMargin, bodyMargin));
        if (ImGui::BeginChild("##SettingsBody", {0.0F, 0.0F}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding()))
        {
            if (compactNavigation)
            {
                const auto currentLabel = [&]() -> std::string_view {
                    switch (m_menu)
                    {
                        case Menu::InputStatus: return T("Sidebar.InputStatus");
                        case Menu::Display: return T("Sidebar.Display");
                        case Menu::FontBuilder: return T("Sidebar.FontBuilder");
                        case Menu::Advanced: return T("Sidebar.Advanced");
                    }
                    return T("Sidebar.InputStatus");
                };
                if (ImGuiEx::M3::BeginCombo("##SettingsNavigation", currentLabel()))
                {
                    const auto menuItem = [&](const std::string_view label, const Menu menu) {
                        if (ImGuiEx::M3::MenuItem(label, m_menu == menu)) m_menu = menu;
                    };
                    menuItem(T("Sidebar.InputStatus"), Menu::InputStatus);
                    menuItem(T("Sidebar.Display"), Menu::Display);
                    menuItem(T("Sidebar.FontBuilder"), Menu::FontBuilder);
                    menuItem(T("Sidebar.Advanced"), Menu::Advanced);
                    ImGuiEx::M3::EndCombo();
                }
                ImGuiEx::M3::Divider();
                ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::BodySmall>(T("SaveHint"), ColorRole::onSurfaceVariant);
                ImGui::Dummy({0.0F, m3Styles.GetPixels(M3Spec::dp<4>())});
            }
            else
            {
                DrawSidebar();
                ImGui::SameLine(0.0F, m3Styles.GetPixels(M3Spec::dp<20>()));
            }

            // Content scrolls on its own; the sidebar stays put.
            const auto noPad = ImGuiEx::StyleGuard().Style<ImGuiStyleVar_WindowPadding>(ImVec2(0.0F, 0.0F));
            if (ImGui::BeginChild("##SettingsContent", {0.0F, 0.0F}))
            {
                // --scroll=N targets this child: it is the window that actually
                // scrolls (the page children below are AutoResizeY).
                if (g_scrollTo >= 0.F && ImGui::GetFrameCount() > 60) ImGui::SetScrollY(g_scrollTo);
                switch (m_menu)
                {
                    case Menu::InputStatus: DrawInputStatus(settings, state); break;
                    case Menu::Display: DrawAppearance(settings); break;
                    case Menu::FontBuilder: DrawFontBuilderPlaceholder(); break;
                    case Menu::Advanced: DrawAdvanced(settings); break;
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        if (ImGui::GetFrameCount() == 30)
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            std::fprintf(stderr, "[preview] toolwindow pos=%.0f,%.0f size=%.0fx%.0f\n", wp.x, wp.y, ws.x, ws.y);
        }
    }
    ImGui::End();
}

void MockToolWindow::DrawAppearance(MiniSettings &settings)
{
    auto      &m3Styles   = ImGuiEx::M3::Context::GetM3Styles();
    const auto styleGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(m3Styles.Colors()[ColorRole::surface]);
    if (ImGui::BeginChild("##Appearance", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding().AutoResizeY()))
    {
        if (g_scrollTo >= 0.F && ImGui::GetFrameCount() > 60) ImGui::SetScrollY(g_scrollTo);
        UI::Panels::PageHeader(T("Sidebar.Display"), T("Page.Display.Support"));

        UI::Panels::SectionHeader(T("Appearance.CandidatePreview"));
        if (UI::Panels::BeginSettingsCard("##CandidatePreviewCard")) DrawCandidatePreview(settings.verticalCandidateList);
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(T("Appearance.CandidateWindow"));
        if (UI::Panels::BeginSettingsCard("##CandidateWindowCard"))
        {
            (void)UI::Panels::SettingsToggleRow("##VerticalCandidates", T("Appearance.VerticalCandidateList"), {}, settings.verticalCandidateList);
            (void)UI::Panels::SettingsToggleRow("##LanguageBar", T("Appearance.AutoToggleLanguageBar"), {}, settings.autoToggleLanguageBar);
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(T("Behaviour.Policy"));
        if (UI::Panels::BeginSettingsCard("##PolicyCard")) DrawPositionPolicy(settings);
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(T("Appearance.General"));
        if (UI::Panels::BeginSettingsCard("##GeneralCard"))
        {
            DrawZoomRow();
            DrawLanguagesRow();
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(T("Appearance.Theme"));
        if (UI::Panels::BeginSettingsCard("##ThemeCard"))
        {
            DrawThemeModeRow();
            DrawThemeRow();
        }
        UI::Panels::EndSettingsCard();
    }
    ImGui::EndChild();
}

void MockToolWindow::DrawFontBuilderPlaceholder()
{
    auto      &m3Styles   = ImGuiEx::M3::Context::GetM3Styles();
    const auto styleGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(m3Styles.Colors()[ColorRole::surface]);
    if (ImGui::BeginChild("##FontBuilder", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding()))
    {
        // PREVIEW_HELP_DIALOG=1 mirrors Fonts.cpp's DrawHelpModal with the live zh
        // strings; PREVIEW_DIALOG_GROW_AT=N swaps in the full text at frame N, so a
        // popup whose size was fitted at open gets hit by the same content growth
        // (text role fonts, UI scaling) that the in-game dialog sees.
        if (std::getenv("PREVIEW_HELP_DIALOG") != nullptr)
        {
            static int  s_frame = 0;
            static bool s_open  = true;
            if (s_open)
            {
                ImGui::OpenPopup("什么是字体构建器?");
                s_open = false;
            }
            if (auto dialog = ImGuiEx::M3::DialogModal("什么是字体构建器?"); dialog)
            {
                int growAt = 0;
                if (const char *growEnv = std::getenv("PREVIEW_DIALOG_GROW_AT"); growEnv != nullptr)
                {
                    growAt = std::atoi(growEnv);
                }
                if (growAt == 0 || s_frame < growAt)
                {
                    dialog.SupportingText("字形查询依赖于字体的优先级。", true);
                }
                else
                {
                    dialog.SupportingText("您可以为不同的 Unicode 字符范围（如拉丁语、中日韩文字）分配特定的字体文件。", true);
                    dialog.SupportingText("字形查询依赖于字体的优先级。如果第一个字体中不包含某个字符，系统将按照您定义的顺序在后续字体中查找。", true);
                }
                dialog.ActionButton("应用");
            }
            ++s_frame;
        }
        else if (auto dialog = ImGuiEx::M3::DialogModal("什么是字体构建器?"); dialog)
        {
            dialog.SupportingText("您可以为不同的 Unicode 字符范围（如拉丁语、中日韩文字）分配特定的字体文件。", true);
            dialog.SupportingText("字形查询依赖于字体的优先级。如果第一个字体中不包含某个字符，系统将按照您定义的顺序在后续字体中查找。", true);
            dialog.ActionButton("应用");
        }
        if (auto dialog = ImGuiEx::M3::DialogModal("警告###Warning"); dialog)
        {
            dialog.SupportingText("为确保稳定性和性能，请避免添加过多字体或使用体积过大的字体文件。大字体占用高内存，可能导致卡顿。", true);
            dialog.ActionButton("应用");
        }
        const std::array headerLinks{
            UI::Panels::PageHeaderLink{"##FontBuilderWarning", T("FontBuilder.Warning")},
            UI::Panels::PageHeaderLink{"##FontBuilderHelp", T("FontBuilder.Help")},
        };
        int linkPressed = -1;
        UI::Panels::PageHeader(T("Sidebar.FontBuilder"), T("Page.Fonts.Support"), headerLinks, &linkPressed);
        if (linkPressed == 0) ImGui::OpenPopup("Warning");
        else if (linkPressed == 1) ImGui::OpenPopup("什么是字体构建器?");

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float  margin    = m3Styles.GetPixels(M3Spec::Layout::Compact::Margin);
        // Mirror of FontBuilderPanel::Draw: master-detail from the M3 medium
        // class up, stacked below it.
        if (available.x >= m3Styles.GetPixels(M3Spec::Layout::Medium::Breakpoint))
        {
            const float listWidth = available.x >= m3Styles.GetPixels(M3Spec::Layout::Expanded::Breakpoint)
                                        ? std::min(m3Styles.GetPixels(M3Spec::Layout::Expanded::FixedPaneWidth), available.x * 0.40F)
                                        : available.x * 0.42F;
            DrawMockFontPane({listWidth, available.y});
            ImGui::SameLine(0.0F, margin);
            DrawMockFontResultCard({available.x - listWidth - margin, available.y});
        }
        else
        {
            const float listHeight = std::clamp(available.y * 0.38F, m3Styles.GetPixels(M3Spec::dp<140>()), m3Styles.GetPixels(M3Spec::dp<320>()));
            DrawMockFontPane({available.x, listHeight});
            ImGui::Dummy({0.0F, margin});
            DrawMockFontResultCard({available.x, available.y - listHeight - margin});
        }
    }
    ImGui::EndChild();
}

void MockToolWindow::DrawMockFontPane(const ImVec2 &size)
{
    // Mirror of FontPreviewPanel::Draw: pinned search bar + full-height
    // scrolling font list (static stand-ins for the DWrite enumeration).
    static constexpr std::array<std::string_view, 24> MOCK_FONTS{
        "Arial", "Arial Black", "Arial Bold", "Calibri", "Cambria", "Candara", "Consolas", "Constantia",
        "Corbel", "Georgia", "Impact", "Malgun Gothic", "Meiryo", "Microsoft JhengHei", "Microsoft YaHei",
        "MS Gothic", "Noto Sans CJK SC", "Segoe UI", "Segoe UI Emoji", "SimSun", "Tahoma",
        "Times New Roman", "Verdana", "方正兰亭黑_GBK",
    };
    if (ImGui::BeginChild("##MockFontsView", size, ImGuiChildFlags_None))
    {
        ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
        const bool edited = ImGuiEx::M3::SearchBar("Filter", m_fontFilter.InputBuf, IM_COUNTOF(m_fontFilter.InputBuf), {.icon = ICON_SEARCH, .hintText = T("FontBuilder.Search")});
        ImGui::PopItemFlag();
        if (edited) m_fontFilter.Build();
        if (ImGui::BeginChild("##MockFontsList", {0.0F, 0.0F}, ImGuiChildFlags_None))
        {
            m_fontFilter.Build();
            for (const auto fontName : MOCK_FONTS)
            {
                if (!m_fontFilter.PassFilter(ImGuiEx::TextStart(fontName), ImGuiEx::TextEnd(fontName))) continue;
                const bool selected = m_fontSelected == static_cast<int>(&fontName - MOCK_FONTS.data());
                if (ImGuiEx::M3::MenuItem(fontName, selected))
                {
                    m_fontSelected = static_cast<int>(&fontName - MOCK_FONTS.data());
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void MockToolWindow::DrawMockFontResultCard(const ImVec2 &size)
{
    // Mirror of FontPreviewPanel::DrawResultCard with static content: status
    // strip / scrolling specimen / build queue / bottom actions.
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    if (size.x <= 0.0F || size.y <= 0.0F) return;

    const float padX = m3Styles.GetPixels(M3Spec::List::paddingX);
    const float padY = m3Styles.GetPixels(M3Spec::List::paddingY);
    const float gap  = m3Styles.GetPixels(M3Spec::dp<8>());
    const float contentW = std::max(size.x - padX * 2.0F, 0.0F);

    const auto iconSizing = M3Spec::GetIconButtonSizing(ImGuiEx::M3::Spec::SizeTips::XSMALL, ImGuiEx::M3::Spec::IconButtonWidths::Default);
    const float iconBtnH  = std::max(m3Styles.GetPixels(iconSizing.containerHeight), m3Styles.GetPixels(M3Spec::IconButtonCommon::MinLayoutSize));
    const float iconBtnW  = iconBtnH;

    float statusLineH = 0.0F, queueTitleH = 0.0F, listLineH = 0.0F, queueEmptyH = 0.0F;
    {
        const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::BodySmall>();
        statusLineH = m3Styles.GetLastText().currText.lineHeight;
        queueEmptyH = ImGuiEx::M3::MeasureWrappedText(T("FontBuilder.BuildQueueEmpty"), contentW).y;
    }
    {
        const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::TitleSmall>();
        queueTitleH = m3Styles.GetLastText().currText.lineHeight;
    }
    {
        const auto fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
        listLineH = m3Styles.GetLastText().currText.lineHeight;
    }

    const float statusH      = padY * 2.0F + statusLineH;
    const float queueHeaderH = std::max(queueTitleH, iconBtnH);
    const float queueTitleGap = m3Styles.GetPixels(M3Spec::dp<4>());
    const float queueTopGap   = m3Styles.GetPixels(M3Spec::dp<8>());
    const float queueBotGap   = m3Styles.GetPixels(M3Spec::dp<12>());

    static constexpr std::array<std::string_view, 2> QUEUE_FONTS{"Microsoft YaHei", "Segoe UI Emoji"};
    constexpr int   kMaxVisibleRows = 4;
    const int       rowCount        = static_cast<int>(QUEUE_FONTS.size());
    const float     rowH            = listLineH + m3Styles.GetPixels(M3Spec::dp<8>());
    const float     queueContentH   = rowCount > 0 ? std::min(rowCount, kMaxVisibleRows) * rowH : queueEmptyH;
    const float     queueH          = queueTopGap + queueHeaderH + queueTitleGap + queueContentH + queueBotGap;

    const float actionBtnH = UI::Panels::ButtonHeight(ImGuiEx::M3::Spec::SizeTips::SMALL);
    const float actionsH   = actionBtnH + padY;

    constexpr float kDividerH   = 1.0F;
    const float     fixedH      = statusH + kDividerH + kDividerH + queueH + actionsH;
    const float     minSpecimen = m3Styles.GetPixels(M3Spec::dp<72>());
    const float     specimenH   = std::max(size.y - fixedH, minSpecimen);
    const float     cardH       = std::max(size.y, fixedH + minSpecimen);

    const auto styleGuard = ImGuiEx::StyleGuard()
                                .Color<ImGuiCol_ChildBg>(m3Styles.Colors()[ColorRole::surfaceContainerLowest])
                                .Color<ImGuiCol_Text>(m3Styles.Colors()[ColorRole::onSurface])
                                .Color<ImGuiCol_Border>(m3Styles.Colors()[ColorRole::outlineVariant])
                                .Style<ImGuiStyleVar_ChildRounding>(m3Styles.GetPixels(M3Spec::ShapeCorner::Medium))
                                .Style<ImGuiStyleVar_WindowPadding>(ImVec2(0.0F, 0.0F))
                                .Style<ImGuiStyleVar_ItemSpacing>(ImVec2(0.0F, 0.0F))
                                .Style<ImGuiStyleVar_ChildBorderSize>(1.0F);
    if (!ImGui::BeginChild("##MockFontResultCard", {size.x, cardH}, ImGuiEx::ChildFlags().Borders())) return;
    const ImVec2 cardMin   = ImGui::GetCursorScreenPos();
    const float  cardRight = cardMin.x + size.x;

    {
        ImGui::SetCursorScreenPos({cardMin.x + padX, cardMin.y + padY});
        const float statusIconSize = m3Styles.GetPixels(M3Spec::dp<16>());
        const auto  fontScope      = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::BodySmall>();
        const std::string_view statusIcon{ICON_FILE_CHECK};
        const ImVec2 iconPos(cardMin.x + padX, cardMin.y + padY + (statusLineH - statusIconSize) * 0.5F);
        ImGui::GetWindowDrawList()->AddText(
            m3Styles.IconFont(), statusIconSize, iconPos, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::onSurfaceVariant]),
            statusIcon.data(), statusIcon.data() + statusIcon.size()
        );
        ImGui::Dummy({statusIconSize * 1.5F, statusLineH});
        ImGui::SameLine(0.0F, gap);
        const float maxTextW = std::max(contentW - statusIconSize * 1.5F - gap, ImGui::GetTextLineHeight() * 4.0F);
        const auto  elided   = UI::Panels::ElideText(T("FontBuilder.PreviewingPath"), maxTextW);
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::BodySmall>(
            elided.empty() ? std::string_view{T("FontBuilder.PreviewingPath")} : std::string_view{elided}, ColorRole::onSurfaceVariant);
        ImGui::Dummy({0.0F, padY});
    }
    ImGui::SetCursorScreenPos({cardMin.x, ImGui::GetCursorScreenPos().y});
    ImGuiEx::M3::Divider();

    if (ImGui::BeginChild("##MockFontSpecimen", {0.0F, specimenH}, ImGuiChildFlags_None))
    {
        const float textPadX = m3Styles.GetPixels(M3Spec::TextParagraph::PaddingX);
        ImGui::Indent(textPadX);
        const float wrapWidth = ImGui::GetContentRegionAvail().x;
        ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::BodyLarge>("!@#$%^&*()_+-=[]{}|;':\",.<>?/", ColorRole::onSurface, wrapWidth);
        ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::BodyLarge>("CJK: 繁體中文测试 / 简体中文测试 / 日本語 / 한국어", ColorRole::onSurface, wrapWidth);
        ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::BodyLarge>("Latín: áéíóú ñ  |  FullWidth: ＡＢＣ１２３", ColorRole::onSurface, wrapWidth);
        ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::BodyLarge>("Dovah: Dovahkiin, naal ok zin los vahriin!", ColorRole::onSurface, wrapWidth);
        ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::BodyLarge>("\"I used to be an adventurer like you...\"", ColorRole::onSurface, wrapWidth);
        ImGui::Unindent(textPadX);
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos({cardMin.x, ImGui::GetCursorScreenPos().y});
    ImGuiEx::M3::Divider();

    ImGui::Dummy({0.0F, queueTopGap});
    const float headerY = ImGui::GetCursorScreenPos().y;
    {
        ImGui::SetCursorScreenPos({cardMin.x + padX, headerY + (queueHeaderH - queueTitleH) * 0.5F});
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::TitleSmall>(T("FontBuilder.BuildQueue"), ColorRole::onSurface);

        const float btnsW = iconBtnW * 2.0F + gap;
        ImGui::SetCursorScreenPos({cardRight - padX - btnsW, headerY + (queueHeaderH - iconBtnH) * 0.5F});
        if (ImGuiEx::M3::XSmallIconButton(ICON_EYE, ImGuiEx::M3::Spec::IconButtonColors::Standard)) {}
        ImGuiEx::M3::SetItemToolTip(T("FontBuilder.PreviewBuild"));
        ImGui::SameLine(0.0F, gap);
        if (ImGuiEx::M3::XSmallIconButton(ICON_PLUS, ImGuiEx::M3::Spec::IconButtonColors::Tonal)) {}
        ImGuiEx::M3::SetItemToolTip(T("FontBuilder.Add"));
    }

    const float rowsY = headerY + queueHeaderH + queueTitleGap;
    ImGui::SetCursorScreenPos({cardMin.x + padX, rowsY});
    if (ImGui::BeginChild("##MockBuildQueueRows", {contentW, queueContentH}, ImGuiChildFlags_None))
    {
        const auto fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
        auto      *drawList  = ImGui::GetWindowDrawList();
        const float numGap   = m3Styles.GetPixels(M3Spec::dp<8>());
        const float rowsMinX = ImGui::GetCursorScreenPos().x;
        const float rowsTop  = ImGui::GetCursorScreenPos().y;
        for (int i = 0; i < rowCount; ++i)
        {
            const ImVec2 rowMin(rowsMinX, rowsTop + static_cast<float>(i) * rowH);
            ImGui::SetCursorScreenPos(rowMin);
            ImGui::Dummy({contentW, rowH});
            const std::string number = std::format("{}.", i + 1);
            const float       numW   = ImGui::CalcTextSize(number.c_str()).x;
            const float       textY  = rowMin.y + ImGuiEx::M3::CenteredTextOffsetY(rowH);
            drawList->AddText({rowMin.x, textY}, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::onSurfaceVariant]), number.c_str());
            drawList->AddText({rowMin.x + numW + numGap, textY}, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::onSurface]),
                              std::string{QUEUE_FONTS[static_cast<size_t>(i)]}.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos({cardMin.x + padX, cardMin.y + cardH - padY - actionBtnH});
    {
        ImGuiEx::M3::ButtonConfiguration config;
        config.Filled().Icon(ICON_CHECK);
        config.Size(ImGuiEx::M3::Spec::SizeTips::SMALL);
        if (ImGuiEx::M3::Button(T("FontBuilder.SetAsDefault"), config)) {}
    }
    ImGui::SameLine(0.0F, gap);
    {
        ImGuiEx::M3::ButtonConfiguration config;
        config.Text().Icon(ICON_ROTATE_CCW);
        config.Size(ImGuiEx::M3::Spec::SizeTips::SMALL);
        if (ImGuiEx::M3::Button(T("FontBuilder.ResetBuilder"), config)) {}
    }

    ImGui::EndChild();
}

void MockToolWindow::DrawCandidatePreview(const bool vertical)
{
    auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float height   = m3Styles.GetPixels(vertical ? M3Spec::dp<148>() : M3Spec::dp<84>());
    const auto  row      = UI::Panels::BeginPlainSettingsRow({}, {}, 0.0F, height);
    if (!row) return;

    auto       *drawList = ImGui::GetWindowDrawList();
    const float inset    = m3Styles.GetPixels(M3Spec::dp<8>());
    const ImVec2 min(row.contentX, row.contentY);
    const ImVec2 max(row.trailingRight, row.contentY + height - inset);
    const float rounding = m3Styles.GetPixels(M3Spec::ShapeCorner::Medium);
    drawList->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::surfaceContainerLow]), rounding);
    drawList->AddRect(min, max, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::outlineVariant]), rounding);

    const auto drawText = [&](const std::string_view text, const ImVec2 position, const ColorRole role) {
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[role]), text.data(), text.data() + text.size());
    };
    const float lineHeight = ImGui::GetTextLineHeight();
    const float innerX     = min.x + inset;
    const float innerY     = min.y + inset;
    if (vertical)
    {
        // Mirrors ImeWindow's vertical candidate rows: full-width rows with the
        // selected entry on a primary pill — the same pill as the horizontal
        // row below, with onPrimary text.
        constexpr std::array<std::string_view, 6> candidates{"1. 你好", "2. hello", "3. こんにちは", "4. 안녕", "5. Hallo", "6. Привет"};
        const float pillPadY  = m3Styles.GetPixels(M3Spec::dp<4>());
        const float pillWidth = max.x - inset - innerX;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            const float y = innerY + static_cast<float>(i) * (lineHeight + m3Styles.GetPixels(M3Spec::dp<2>()));
            if (i == 0)
            {
                drawList->AddRectFilled({innerX, y - pillPadY}, {innerX + pillWidth, y + lineHeight + pillPadY}, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::primary]), m3Styles.GetPixels(M3Spec::ShapeCorner::Small));
            }
            drawText(candidates[i], {innerX + m3Styles.GetPixels(M3Spec::dp<12>()), y + (i == 0 ? ImGuiEx::M3::CenteredTextOffsetY(lineHeight + pillPadY * 2.F) - pillPadY : 0.F)}, i == 0 ? ColorRole::onPrimary : ColorRole::onSurface);
        }
    }
    else
    {
        // Mirrors ImeWindow's horizontal candidate row: dim digit hugging the
        // word, selected entry on a filled primary pill.
        drawText("SimpleIME", {innerX, innerY}, ColorRole::onSurfaceVariant);
        constexpr std::array<std::pair<std::string_view, std::string_view>, 6> candidates{
            {{"1", "你好"}, {"2", "hello"}, {"3", "こんにちは"}, {"4", "안녕"}, {"5", "Hallo"}, {"6", "Привет"}}
        };
        const float gap      = m3Styles.GetPixels(M3Spec::dp<10>());
        const float padX     = m3Styles.GetPixels(M3Spec::dp<8>());
        const float padY     = m3Styles.GetPixels(M3Spec::dp<4>());
        const float y        = innerY + lineHeight + m3Styles.GetPixels(M3Spec::dp<8>()) + padY;
        // The pill is symmetric around the line box, but CJK ink hangs below
        // that box's optical middle; pin text with the same shared optical
        // centering offset ImeWindow's candidate row uses.
        const float textY    = y - padY + ImGuiEx::M3::CenteredTextOffsetY(lineHeight + padY * 2.F);
        float       x        = innerX;
        const float maxWidth = max.x - inset;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            const auto &[number, word] = candidates[i];
            const float numWidth  = ImGui::CalcTextSize(number.data(), number.data() + number.size()).x;
            const float wordWidth = ImGui::CalcTextSize(word.data(), word.data() + word.size()).x;
            const float width     = numWidth + wordWidth + padX * 2.F;
            if (x + width > maxWidth) break;
            if (i == 0)
            {
                drawList->AddRectFilled({x, y - padY}, {x + width, y + lineHeight + padY}, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::primary]), m3Styles.GetPixels(M3Spec::ShapeCorner::Small));
                drawText(number, {x + padX, textY}, ColorRole::onPrimary);
                drawText(word, {x + padX + numWidth, textY}, ColorRole::onPrimary);
            }
            else
            {
                drawText(number, {x + padX, textY}, ColorRole::onSurfaceVariant);
                drawText(word, {x + padX + numWidth, textY}, ColorRole::onSurface);
            }
            x += width + gap;
        }
    }
    UI::Panels::EndSettingsRow(row);
}

void MockToolWindow::DrawZoomRow()
{
    namespace M3 = ImGuiEx::M3;

    // Stress mode shows the longest possible preview ("follow monitor"
    // translation, doubled) so the chip's elision path gets exercised.
    const std::string preview =
        std::getenv("PREVIEW_LANG_STRESS") != nullptr ? std::string(T("Appearance.ZoomFollowMonitor")) : std::string("100%");
    // Reserve the combo's exact width so the row title wraps short of the chip
    // instead of colliding under long translations (port of AppearancePanel).
    const float padX   = M3::Context::GetM3Styles().GetPixels(M3Spec::List::paddingX);
    const float comboW = UI::Panels::RowTrailingComboWidth(preview, ImGui::GetContentRegionAvail().x - padX * 2.0F);
    const auto  row    = UI::Panels::BeginPlainSettingsRow(T("Appearance.Zoom"), {}, comboW, UI::Panels::ComboButtonHeight());
    if (!row)
    {
        return;
    }
    UI::Panels::RowTitle(row, T("Appearance.Zoom"));
    const bool open = UI::Panels::BeginRowTrailingCombo(row, "##ZoomCombo", preview);
    if (open)
    {
        // Item list same shape as the real panel; values are stubs here.
        (void)M3::MenuItem("100%", true);
    }
    UI::Panels::EndRowTrailingCombo(open);
    UI::Panels::EndSettingsRow(row);
}

void MockToolWindow::DrawLanguagesRow()
{
    // Reserve the combo's exact width so the title never runs under the chip
    // (port of AppearancePanel's languages row).
    const float padX   = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::List::paddingX);
    const float comboW = UI::Panels::RowTrailingComboWidth("中文", ImGui::GetContentRegionAvail().x - padX * 2.0F);
    const auto row = UI::Panels::BeginPlainSettingsRow(T("Appearance.Languages"), {}, comboW, UI::Panels::ComboButtonHeight());
    if (!row)
    {
        return;
    }
    UI::Panels::RowTitle(row, T("Appearance.Languages"));
    const bool open = UI::Panels::BeginRowTrailingCombo(row, "##LanguagesCombo", "中文");
    if (open)
    {
        (void)ImGuiEx::M3::MenuItem("中文", true);
        (void)ImGuiEx::M3::MenuItem("english", false);
    }
    UI::Panels::EndRowTrailingCombo(open);
    UI::Panels::EndSettingsRow(row);
}

void MockToolWindow::DrawThemeModeRow()
{
    using ImGuiEx::M3::Spec::ButtonShape;
    using ImGuiEx::M3::Spec::SizeTips;

    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    const float gap     = m3Styles.GetPixels(M3Spec::List::segmentedGap);
    const float rowH    = UI::Panels::ButtonHeight();
    const float lineGap = m3Styles.GetPixels(M3Spec::dp<8>());
    const float padX    = m3Styles.GetPixels(M3Spec::List::paddingX);

    const std::string_view title = T("Appearance.ThemeMode");
    const std::array<std::string_view, 2> labels{T("Appearance.Light"), T("Appearance.Dark")};
    std::vector<float> widths;
    widths.reserve(labels.size());
    for (const auto label : labels)
    {
        widths.push_back(UI::Panels::MeasureButton(label, {}, SizeTips::XSMALL, ButtonShape::Square));
    }

    const auto segment = [&](const std::string_view label, const bool selectDark) {
        ImGuiEx::M3::ButtonConfiguration config;
        config.Tonal().Square();
        config.toggle   = true;
        config.Size(SizeTips::XSMALL);
        config.selected = m3Styles.Colors().IsDark() == selectDark;
        if (ImGuiEx::M3::Button(label, config) && m3Styles.Colors().IsDark() != selectDark)
        {
            m3Styles.ToggleLightDarkScheme();
            ImGuiEx::M3::SetupDefaultImGuiStyles(ImGui::GetStyle());
        }
    };

    // The title and the segment run share the row; when the run would pass
    // the card, it moves below the title and flows (port of AppearancePanel).
    const float titleW  = [&] {
        const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::List::textRole>();
        return ImGui::CalcTextSize(title.data(), title.data() + title.size()).x;
    }();
    const float contentW = ImGui::GetContentRegionAvail().x - padX * 2.0F;
    const float runW     = widths[0] + gap + widths[1];
    const bool  stacked  = titleW + gap + runW > contentW;

    if (stacked)
    {
        const float titleLineH = [&] {
            const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::List::textRole>();
            return m3Styles.GetLastText().currText.lineHeight;
        }();
        const float titleGap = m3Styles.GetPixels(M3Spec::dp<4>());
        const auto  flow     = UI::Panels::FlowLayoutPositions({}, contentW, gap, lineGap, rowH, widths);
        const auto  row      = UI::Panels::BeginPlainSettingsRow(T("Appearance.ThemeMode"), {}, 0.0F, titleLineH + titleGap + flow.height);
        if (!row)
        {
            return;
        }
        UI::Panels::RowTitle(row, title);
        for (size_t i = 0; i < labels.size(); ++i)
        {
            ImGui::SetCursorScreenPos({row.contentX + flow.positions[i].x, row.contentY + titleLineH + titleGap + flow.positions[i].y});
            segment(labels[i], i == 1);
        }
        UI::Panels::EndSettingsRow(row);
    }
    else
    {
        const auto row = UI::Panels::BeginPlainSettingsRow(T("Appearance.ThemeMode"), {}, runW, rowH);
        if (!row)
        {
            return;
        }
        UI::Panels::RowTitle(row, title);
        ImGui::SetCursorScreenPos({row.trailingRight - runW, row.centerY - rowH * 0.5F});
        segment(labels[0], false);
        ImGui::SameLine(0.0F, gap);
        segment(labels[1], true);
        ImGui::NewLine();
        UI::Panels::EndSettingsRow(row);
    }
}

void MockToolWindow::DrawThemeRow()
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    const std::string_view customizeLabel = T("Appearance.Customize");
    const std::string_view resetLabel     = T("Appearance.ResetTheme");
    const float            swatchSize      = m3Styles.GetPixels(M3Spec::dp<40>());
    const float            buttonHeight    = UI::Panels::ButtonHeight();
    const float            buttonGap       = m3Styles.GetPixels(M3Spec::dp<8>());
    const float            customizeWidth  = UI::Panels::MeasureButton(customizeLabel, ICON_PALETTE);
    const float            resetWidth      = UI::Panels::TextLinkWidth(resetLabel);
    const float            resetHeight     = UI::Panels::TextLinkHeight();
    const float            trailingReserve = customizeWidth + buttonGap + resetWidth;

    const auto row = UI::Panels::BeginPlainSettingsRow({}, {}, trailingReserve, swatchSize);
    if (row)
    {
        ImGui::SetCursorScreenPos({row.contentX, row.centerY - swatchSize * 0.5F});
        (void)ImGui::ColorButton(
            "##SourceColor", ImGuiEx::M3::ArgbToImVec4(0xFF4D7A6D), ImGuiEx::ColorEditFlags().NoAlpha().AlphaOpaque().NoPicker().NoTooltip(),
            {swatchSize, swatchSize}
        );

        UI::Panels::RowTitle(row, T("Appearance.Theme"), swatchSize + m3Styles.GetPixels(M3Spec::dp<16>()));

        ImGui::SetCursorScreenPos({row.trailingRight - trailingReserve, row.centerY - buttonHeight * 0.5F});
        (void)ImGuiEx::M3::XSmallButton(customizeLabel, ICON_PALETTE);
        ImGui::SetCursorScreenPos({row.trailingRight - resetWidth, row.centerY - resetHeight * 0.5F});
        (void)UI::Panels::TextLink("##ResetTheme", resetLabel);

        UI::Panels::EndSettingsRow(row);
    }
}

void MockToolWindow::DrawInputStatus(MiniSettings &settings, const MiniState &state)
{
    const auto pageGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(ImGuiEx::M3::Context::GetM3Styles().Colors()[ColorRole::surface]);
    if (ImGui::BeginChild("InputStatus", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding().AutoResizeY()))
    {
        if (g_scrollTo >= 0.F && ImGui::GetFrameCount() > 60) ImGui::SetScrollY(g_scrollTo);
        UI::Panels::PageHeader(T("Sidebar.InputStatus"), T("Page.InputStatus.Support"));
        DrawStatusCard(settings, state);

        UI::Panels::SectionHeader(T("Behaviour.Input"));
        if (UI::Panels::BeginSettingsCard("##InputCard"))
        {
            bool enableMod = settings.enableMod;
            (void)UI::Panels::SettingsToggleRow("##EnableMod", T("Behaviour.EnableMod"), T("Behaviour.EnableModToolTip"), enableMod, SupportingMeasure());
            settings.enableMod = enableMod;

            // The three dependent switches collapse into one quiet summary row:
            // a status dot + text link per entry, wrapping when the width runs
            // out (port of ToolWindow.cpp's summary row).
            ImGui::BeginDisabled(!settings.enableMod);
            ImGuiEx::M3::Divider();
            {
                auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
                const float dotGap  = m3Styles.GetPixels(M3Spec::dp<6>());
                const float itemGap = m3Styles.GetPixels(M3Spec::dp<16>());
                const float lineGap = m3Styles.GetPixels(M3Spec::dp<4>());
                const float linkH   = UI::Panels::TextLinkHeight();
                const float padX    = m3Styles.GetPixels(M3Spec::List::paddingX);

                float                dotW = 0.0F;
                std::array<float, 3> itemW{};
                {
                    const auto fontScope = m3Styles.UseTextRole<M3Spec::TextRole::LabelLarge>();
                    dotW = ImGui::GetTextLineHeight() * 0.6F;
                    itemW[0] = dotW + dotGap + UI::Panels::TextLinkWidth(T("Behaviour.ShortAutoToggleKeyboard"));
                    itemW[1] = dotW + dotGap + UI::Panels::TextLinkWidth(T("Behaviour.ShortUnicodePaste"));
                    itemW[2] = dotW + dotGap + UI::Panels::TextLinkWidth(T("Behaviour.KeepImeOpen"));
                }
                const float contentW = ImGui::GetContentRegionAvail().x - padX * 2.0F;
                const auto  flow     = UI::Panels::FlowLayoutPositions({}, contentW, itemGap, lineGap, linkH, {itemW.begin(), itemW.end()});

                if (const auto row = UI::Panels::BeginPlainSettingsRow({}, {}, 0.0F, flow.height); row)
                {
                    float x = 0.0F;
                    const auto item = [&](const size_t index, const char *strId, const std::string_view label, bool &value, const std::string_view tooltip) {
                        x = row.contentX + flow.positions[index].x;
                        const float y = row.contentY + flow.positions[index].y;
                        {
                            const auto fontScope = m3Styles.UseTextRole<M3Spec::TextRole::LabelLarge>();
                            ImGui::SetCursorScreenPos({x, y + (linkH - ImGui::GetTextLineHeight()) * 0.5F});
                            UI::Panels::StatusDot(value);
                        }
                        x += dotW + dotGap;
                        ImGui::SetCursorScreenPos({x, y});
                        if (UI::Panels::TextLink(strId, label, value ? ColorRole::onSurface : ColorRole::onSurfaceVariant))
                        {
                            value = !value;
                        }
                        ImGuiEx::M3::SetItemToolTip(tooltip);
                    };
                    item(0, "##AutoToggleKeyboard", T("Behaviour.ShortAutoToggleKeyboard"), settings.autoToggleKeyboard, T("Behaviour.AutoToggleKeyboardToolTip"));
                    item(1, "##UnicodePaste", T("Behaviour.ShortUnicodePaste"), settings.enableUnicodePaste, T("Behaviour.EnableUnicodePasteToolTip"));
                    item(2, "##KeepImeOpen", T("Behaviour.KeepImeOpen"), settings.keepImeOpen, T("Behaviour.KeepImeOpenTooltip"));
                    UI::Panels::EndSettingsRow(row);
                }
            }
            ImGui::EndDisabled();
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(T("Behaviour.Shortcut"));
        if (UI::Panels::BeginSettingsCard("##ShortcutCard")) DrawShortcutSection(settings);
        UI::Panels::EndSettingsCard();

        // Port of ToolWindow.cpp's compatibility card: bridge switches plus
        // install-time captions; bridge states come from MiniState ints.
        UI::Panels::SectionHeader(T("Behaviour.Compatibility"));
        if (UI::Panels::BeginSettingsCard("##CompatibilityCard"))
        {
            const auto compatRow = [&](const char *strId, bool &value, const std::string_view title, const std::string_view supporting, const int state) {
                const auto [caption, captionColor] = BridgeCaption(value, state);
                auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
                float captionW = 0.0F;
                {
                    const auto fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
                    captionW = ImGui::CalcTextSize(caption.data(), caption.data() + caption.size()).x;
                }
                const float trailing = captionW + m3Styles.GetPixels(M3Spec::dp<12>()) + UI::Panels::SwitchReserve();
                const auto row = UI::Panels::BeginSettingsRow(strId, title, supporting, trailing, m3Styles.GetPixels(M3Spec::dp<24>()), SupportingMeasure());
                if (row)
                {
                    UI::Panels::RowTitle(row, title);
                    UI::Panels::RowSupporting(row, supporting);
                    UI::Panels::RowTrailingText(row, caption, captionColor, UI::Panels::SwitchReserve());
                    UI::Panels::RowTrailingSwitch(row, value);
                    UI::Panels::EndSettingsRow(row);
                }
                if (row.pressed) value = !value;
            };
            compatRow("##MeridianSupport", settings.meridianSupport, T("Behaviour.MeridianSupport"), T("Behaviour.MeridianSupportToolTip"), state.meridianBridge);
            UI::Panels::RowDivider();
            compatRow("##PrismaAvoidance", settings.prismaAvoidance, T("Behaviour.PrismaAvoidance"), T("Behaviour.PrismaAvoidanceToolTip"), state.prismaBridge);
            UI::Panels::RowDivider();
            compatRow("##SkseMenuFrameworkSupport", settings.skseMenuFrameworkSupport, T("Behaviour.SkseMenuFrameworkSupport"), T("Behaviour.SkseMenuFrameworkSupportToolTip"), state.sksemfBridge);
        }
        UI::Panels::EndSettingsCard();
    }
    ImGui::EndChild();
}

void MockToolWindow::DrawAdvanced(MiniSettings &settings)
{
    const auto pageGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(ImGuiEx::M3::Context::GetM3Styles().Colors()[ColorRole::surface]);
    if (ImGui::BeginChild("Advanced", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding().AutoResizeY()))
    {
        UI::Panels::PageHeader(T("Sidebar.Advanced"), T("Page.Advanced.Support"));
        UI::Panels::SectionHeader(T("Advanced.Troubleshooting"));
        if (UI::Panels::BeginSettingsCard("##TroubleshootingCard"))
        {
            ImGui::BeginDisabled(!settings.enableMod);
            (void)UI::Panels::SettingsToggleRow("##FixInconsistent", T("Behaviour.FixInconsistent"), T("Behaviour.FixInconsistentToolTip"), settings.fixInconsistentTextEntry, SupportingMeasure());
            ImGui::EndDisabled();

            ImGuiEx::M3::Divider();
            // Stub of ToolWindow.cpp's log-path row (copy + open): copies a mock location.
            const std::string_view copyLabel   = T("Advanced.CopyLogPath");
            const std::string_view copiedLabel = T("Advanced.Copied");
            const std::string_view openLabel   = T("Open");
            const float            linkH       = UI::Panels::TextLinkHeight();
            const float            linkGap     = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::dp<8>());
            const float            copyW       = UI::Panels::TextLinkWidth(copyLabel);
            const float            copiedW     = UI::Panels::TextLinkWidth(copiedLabel);
            const float            openW       = UI::Panels::TextLinkWidth(openLabel);
            const bool             copied      = m_copiedAt >= 0.0F && ImGui::GetTime() - m_copiedAt < 1.5;
            const float            reserveW    = std::max(copyW, copiedW) + linkGap + openW;
            if (const auto row = UI::Panels::BeginPlainSettingsRow(T("Advanced.LogFile"), {}, reserveW, linkH); row)
            {
                UI::Panels::RowTitle(row, T("Advanced.LogFile"));
                ImGui::SetCursorScreenPos({row.trailingRight - reserveW, row.centerY - linkH * 0.5F});
                if (UI::Panels::TextLink("##CopyLogPath", copied ? copiedLabel : copyLabel))
                {
                    ImGui::SetClipboardText("C:\\Games\\Skyrim\\Data\\interface\\SimpleIME\\SimpleIME.log");
                    m_copiedAt = static_cast<float>(ImGui::GetTime());
                }
                ImGui::SetCursorScreenPos({row.trailingRight - openW, row.centerY - linkH * 0.5F});
                (void)UI::Panels::TextLink("##OpenLogDir", openLabel);
                UI::Panels::EndSettingsRow(row);
            }

            ImGuiEx::M3::Divider();
            // Port of ToolWindow.cpp's diagnostics row: copies a sample report.
            const std::string_view diagCopyLabel = T("Copy");
            const float            diagCopyW     = UI::Panels::TextLinkWidth(diagCopyLabel);
            const float            diagCopiedW   = UI::Panels::TextLinkWidth(copiedLabel);
            const bool             diagCopied    = m_diagCopiedAt >= 0.0F && ImGui::GetTime() - m_diagCopiedAt < 1.5;
            const float            diagReserveW  = std::max(diagCopyW, diagCopiedW);
            if (const auto row = UI::Panels::BeginPlainSettingsRow(T("Advanced.Diagnostics"), T("Advanced.DiagnosticsSupport"), diagReserveW, linkH, SupportingMeasure()); row)
            {
                UI::Panels::RowTitle(row, T("Advanced.Diagnostics"));
                UI::Panels::RowSupporting(row, T("Advanced.DiagnosticsSupport"));
                ImGui::SetCursorScreenPos({row.trailingRight - diagReserveW, row.centerY - linkH * 0.5F});
                if (UI::Panels::TextLink("##CopyDiagnostics", diagCopied ? copiedLabel : diagCopyLabel))
                {
                    ImGui::SetClipboardText("SimpleIME 2.4.0\nSKSE 2.2.6\nMeridian UI: active\n...");
                    m_diagCopiedAt = static_cast<float>(ImGui::GetTime());
                }
                UI::Panels::EndSettingsRow(row);
            }
        }
        UI::Panels::EndSettingsCard();

        // Port of ToolWindow.cpp's Logging & error messages card.
        UI::Panels::SectionHeader(T("Advanced.Logging"));
        if (UI::Panels::BeginSettingsCard("##LoggingCard"))
        {
            static constexpr std::array<const char *, 7> kLevelNames{"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};
            {
                const std::string preview = kLevelNames[settings.logLevel];
                const float       padX    = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::List::paddingX);
                const float       comboW  = UI::Panels::RowTrailingComboWidth(preview, ImGui::GetContentRegionAvail().x - padX * 2.0F);
                if (const auto row = UI::Panels::BeginPlainSettingsRow(T("Advanced.LogLevel"), T("Advanced.LogLevelSupport"), comboW, UI::Panels::ComboButtonHeight(), SupportingMeasure()); row)
                {
                    UI::Panels::RowTitle(row, T("Advanced.LogLevel"));
                    UI::Panels::RowSupporting(row, T("Advanced.LogLevelSupport"));
                    const bool open = UI::Panels::BeginRowTrailingCombo(row, "##LogLevelCombo", preview);
                    if (open)
                    {
                        for (int level = 0; level < static_cast<int>(kLevelNames.size()); ++level)
                        {
                            const bool selected = level == settings.logLevel;
                            if (ImGuiEx::M3::MenuItem(kLevelNames[level], selected) && !selected)
                            {
                                settings.logLevel = level;
                            }
                        }
                    }
                    UI::Panels::EndRowTrailingCombo(open);
                    UI::Panels::EndSettingsRow(row);
                }
            }
            ImGuiEx::M3::Divider();
            {
                static constexpr std::array<int, 5> kDurations{-1, 5, 10, 30, 60};
                const std::string preview = settings.errorDuration < 0 ? std::string(T("Advanced.ErrorDurationNever")) : std::format("{} s", settings.errorDuration);
                const float       padX    = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::List::paddingX);
                const float       comboW  = UI::Panels::RowTrailingComboWidth(preview, ImGui::GetContentRegionAvail().x - padX * 2.0F);
                if (const auto row = UI::Panels::BeginPlainSettingsRow(T("Advanced.ErrorDuration"), T("Advanced.ErrorDurationSupport"), comboW, UI::Panels::ComboButtonHeight(), SupportingMeasure()); row)
                {
                    UI::Panels::RowTitle(row, T("Advanced.ErrorDuration"));
                    UI::Panels::RowSupporting(row, T("Advanced.ErrorDurationSupport"));
                    const bool open = UI::Panels::BeginRowTrailingCombo(row, "##ErrorDurationCombo", preview);
                    if (open)
                    {
                        for (const int duration : kDurations)
                        {
                            const bool selected = duration == settings.errorDuration;
                            const std::string label = duration < 0 ? std::string(T("Advanced.ErrorDurationNever")) : std::format("{} s", duration);
                            if (ImGuiEx::M3::MenuItem(label, selected) && !selected)
                            {
                                settings.errorDuration = duration;
                            }
                        }
                    }
                    UI::Panels::EndRowTrailingCombo(open);
                    UI::Panels::EndSettingsRow(row);
                }
            }
        }
        UI::Panels::EndSettingsCard();

        // Port of ToolWindow.cpp's Runtime environment card (read-only rows).
        UI::Panels::SectionHeader(T("Advanced.Environment"));
        if (UI::Panels::BeginSettingsCard("##EnvironmentCard"))
        {
            const auto drawValueRow = [](const std::string_view title, const std::string_view supporting, const std::string_view value) {
                float valueW = 0.0F;
                {
                    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
                    const auto fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
                    valueW = ImGui::CalcTextSize(value.data(), value.data() + value.size()).x + m3Styles.GetPixels(M3Spec::dp<12>());
                }
                if (const auto row = UI::Panels::BeginPlainSettingsRow(title, supporting, valueW, 0.0F, SupportingMeasure()); row)
                {
                    UI::Panels::RowTitle(row, title);
                    UI::Panels::RowSupporting(row, supporting);
                    UI::Panels::RowTrailingText(row, value, ColorRole::onSurfaceVariant);
                    UI::Panels::EndSettingsRow(row);
                }
            };
            drawValueRow(T("Advanced.DpiAwareness"), T("Advanced.DpiAwarenessSupport"), settings.forceDpiAwareness ? T("Advanced.StateEnabled") : T("Advanced.StateDisabled"));
            ImGuiEx::M3::Divider();
            drawValueRow(T("Advanced.TsfFramework"), T("Advanced.TsfFrameworkSupport"), settings.enableTsf ? std::string_view{"TSF"} : std::string_view{"Imm32"});
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(T("Advanced.ConfigTitle"));
        if (UI::Panels::BeginSettingsCard("##ConfigOnlyCard"))
        {
            const std::string_view openLabel = T("Open");
            const float            openW     = UI::Panels::TextLinkWidth(openLabel);
            const float            linkH     = UI::Panels::TextLinkHeight();
            const auto row = UI::Panels::BeginPlainSettingsRow(T("Advanced.ConfigPath"), T("Advanced.ConfigDescription"), openW, linkH, SupportingMeasure());
            if (row)
            {
                UI::Panels::RowTitle(row, T("Advanced.ConfigPath"));
                UI::Panels::RowSupporting(row, T("Advanced.ConfigDescription"));
                ImGui::SetCursorScreenPos({row.trailingRight - openW, row.centerY - linkH * 0.5F});
                (void)UI::Panels::TextLink("##OpenConfigDir", openLabel);
                UI::Panels::EndSettingsRow(row);
            }

            // Stub of ToolWindow.cpp's config-status row: PREVIEW_MOCK_CONFIG_STATUS=
            // error|missing flips the state so the failure rendering can be captured.
            const char *mode = std::getenv("PREVIEW_MOCK_CONFIG_STATUS");
            const bool  error = mode != nullptr && std::strcmp(mode, "error") == 0;
            const bool  missing = mode != nullptr && std::strcmp(mode, "missing") == 0;
            std::string_view label = T("Advanced.ConfigStatusOk");
            ColorRole        labelColor = ColorRole::primary;
            if (error)
            {
                label = T("Advanced.ConfigStatusError");
                labelColor = ColorRole::error;
            }
            else if (missing)
            {
                label = T("Advanced.ConfigStatusMissing");
                labelColor = ColorRole::onSurfaceVariant;
            }
            ImGuiEx::M3::Divider();
            // Port of ToolWindow.cpp's config-status row: reserve the trailing
            // label's width so the title wraps short of it, and pass the
            // supporting copy only when one is rendered.
            auto  &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
            float statusW = 0.0F;
            {
                const auto fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
                statusW = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + m3Styles.GetPixels(M3Spec::dp<12>());
            }
            const auto statusRow = UI::Panels::BeginPlainSettingsRow(
                T("Advanced.ConfigStatus"), error ? "toml::parse_error: unexpected key at line 12" : std::string_view{}, statusW, 0.0F,
                SupportingMeasure());
            if (statusRow)
            {
                UI::Panels::RowTitle(statusRow, T("Advanced.ConfigStatus"));
                if (error)
                {
                    UI::Panels::RowSupporting(statusRow, "toml::parse_error: unexpected key at line 12");
                }
                UI::Panels::RowTrailingText(statusRow, label, labelColor);
                UI::Panels::EndSettingsRow(statusRow);
            }
        }
        UI::Panels::EndSettingsCard();
    }
    ImGui::EndChild();
}

void MockToolWindow::DrawStatusCard(MiniSettings &settings, const MiniState &state)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    if (const auto card = UI::Panels::BeginSettingsCard("##StatusCard", ColorRole::primaryContainer, false); card)
    {
        const float  padX   = m3Styles.GetPixels(M3Spec::List::paddingX);
        const float  padY   = m3Styles.GetPixels(M3Spec::dp<12>());
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;

        ImGui::SetCursorScreenPos({origin.x + padX, origin.y + padY});
        ImGuiEx::M3::TextUnformatted<M3Spec::TextRole::TitleSmall>(T("Behaviour.LiveStatus"), ColorRole::onPrimaryContainer);
        ImGui::Dummy({0.0F, m3Styles.GetPixels(M3Spec::dp<8>())});

        const auto modLabel = settings.enableMod ? T("Behaviour.StatusModOn") : T("Behaviour.StatusModOff");
        const auto imeLabel = state.imeOn ? T("Behaviour.StatusImeOn") : T("Behaviour.StatusImeOff");
        const std::string_view focusLabel = "请求焦点";

        // Measure every entry before the row's extent commits (port of
        // ToolWindow.cpp's flow layout — long translations wrap, not collide).
        float dotW  = 0.0F;
        float modW  = 0.0F;
        float imeW  = 0.0F;
        float linkW = 0.0F;
        float linkH = 0.0F;
        {
            const auto fontScope = m3Styles.UseTextRole<M3Spec::TextRole::LabelLarge>();
            dotW  = ImGui::GetTextLineHeight() * 0.6F;
            modW  = ImGui::CalcTextSize(modLabel.data(), modLabel.data() + modLabel.size()).x;
            imeW  = ImGui::CalcTextSize(imeLabel.data(), imeLabel.data() + imeLabel.size()).x;
            linkW = UI::Panels::TextLinkWidth(focusLabel);
            linkH = UI::Panels::TextLinkHeight();
        }
        const float dotGap   = m3Styles.GetPixels(M3Spec::dp<8>());
        const float chipGap  = m3Styles.GetPixels(M3Spec::dp<16>());
        const float lineGap  = m3Styles.GetPixels(M3Spec::dp<8>());
        const float chipH    = m3Styles.GetPixels(M3Spec::dp<32>());
        const float lineH    = std::max(chipH, linkH);
        const float contentW = width - padX * 2.0F;
        const float contentTop = ImGui::GetCursorScreenPos().y;
        const auto flow = UI::Panels::FlowLayoutPositions(
            {origin.x + padX, contentTop}, contentW, chipGap, lineGap, lineH, {dotW + dotGap + modW, dotW + dotGap + imeW, linkW}
        );

        const auto chip = [&](const ImVec2 &position, const bool on, const std::string_view label, const std::string_view tooltip) {
            const auto fontScope = m3Styles.UseTextRole<M3Spec::TextRole::LabelLarge>();
            const auto &typeScale = m3Styles.GetLastText().currText;
            ImGui::SetCursorScreenPos({position.x, position.y + (chipH - typeScale.lineHeight) * 0.5F});
            UI::Panels::StatusDot(on);
            ImGui::SameLine(0.0F, dotGap);
            ImGuiEx::M3::AlignedLabel(label, ColorRole::onPrimaryContainer);
            if (!tooltip.empty())
            {
                ImGuiEx::M3::SetItemToolTip(tooltip);
            }
        };

        chip(flow.positions[0], settings.enableMod, modLabel, {});
        chip(flow.positions[1], state.imeOn, imeLabel, "IME 默认仅在存在输入框时启用");

        ImGui::SetCursorScreenPos({flow.positions[2].x, flow.positions[2].y + (lineH - linkH) * 0.5F});
        ImGui::BeginDisabled(state.focus);
        (void)UI::Panels::TextLink("##ForceFocusIme", focusLabel, ColorRole::onPrimaryContainer, ColorRole::primaryContainer);
        ImGui::EndDisabled();

        ImGui::SetCursorScreenPos({origin.x, contentTop + flow.height});
        ImGui::Dummy({0.0F, padY});
    }
    UI::Panels::EndSettingsCard();
}

void MockToolWindow::DrawShortcutSection(MiniSettings &settings)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    if (!settings.enableMod)
    {
        m_capturing = false;
    }
    ImGui::BeginDisabled(!settings.enableMod);

    const float linkHeight = UI::Panels::TextLinkHeight();
    const float linkGap    = m3Styles.GetPixels(M3Spec::dp<4>());

    if (m_capturing)
    {
        const std::string_view cancelLabel = T("Cancel");
        const float            cancelWidth = UI::Panels::TextLinkWidth(cancelLabel);
        const auto             row         = UI::Panels::BeginPlainSettingsRow({}, {}, cancelWidth, linkHeight);
        if (row)
        {
            ImGuiEx::M3::AlignedLabel("请按下新的快捷键…", ColorRole::primary);
            ImGui::SetCursorScreenPos({row.trailingRight - cancelWidth, row.centerY - linkHeight * 0.5F});
            if (UI::Panels::TextLink("##CancelShortcutCapture", cancelLabel))
            {
                m_capturing = false;
            }
            UI::Panels::EndSettingsRow(row);
        }
    }
    else
    {
        const std::string_view changeLabel = T("Behaviour.ChangeShortcut");
        const std::string_view resetLabel  = T("Behaviour.ResetShortcut");
        const float            changeWidth = UI::Panels::TextLinkWidth(changeLabel);
        const float            resetWidth  = UI::Panels::TextLinkWidth(resetLabel);
        const float            keycapsHeight   = ImGui::GetFrameHeight();
        const float            linksReserve  = changeWidth + linkGap + resetWidth;

        // Measure the keycap run (keycaps separated by gap + "+" + gap) so a
        // wide chord can be told apart from the trailing links before the row
        // commits: the two share the line while they fit, and the links drop
        // to a second line when long translations plus a long chord collide
        // (port of ToolWindow.cpp).
        const float keycapSpacing = m3Styles.GetPixels(M3Spec::dp<8>());
        float       keycapsWidth  = 0.0F;
        {
            const auto  fontScope = m3Styles.UseTextRole<M3Spec::TextRole::LabelMedium>();
            const float plusWidth = ImGui::CalcTextSize("+").x;
            bool        firstKey  = true;
            ForEachShortcutKeycap(settings.shortcut, [&](const std::string_view keyLabel) {
                keycapsWidth += (firstKey ? 0.0F : keycapSpacing * 2.0F + plusWidth) + UI::Panels::KeycapWidth(keyLabel);
                firstKey = false;
            });
        }

        const float padX     = m3Styles.GetPixels(M3Spec::List::paddingX);
        const float stackGap = m3Styles.GetPixels(M3Spec::dp<8>());
        const bool  stacked  = keycapsWidth + keycapSpacing + linksReserve > ImGui::GetContentRegionAvail().x - padX * 2.0F;
        const float rowControlHeight = stacked ? keycapsHeight + stackGap + linkHeight : std::max(linkHeight, keycapsHeight);
        const auto  row              = UI::Panels::BeginPlainSettingsRow({}, {}, linksReserve, rowControlHeight);
        if (row)
        {
            const float keycapsY = stacked ? row.contentY : row.centerY - keycapsHeight * 0.5F;
            const float linkY    = stacked ? row.contentY + keycapsHeight + stackGap : row.centerY - linkHeight * 0.5F;

            ImGui::SetCursorScreenPos({row.contentX, keycapsY});
            DrawShortcutKeycaps(settings.shortcut);

            ImGui::SetCursorScreenPos({row.trailingRight - linksReserve, linkY});
            if (UI::Panels::TextLink("##ChangeShortcut", changeLabel))
            {
                m_capturing = true;
            }
            ImGui::SetCursorScreenPos({row.trailingRight - resetWidth, linkY});
            (void)UI::Panels::TextLink("##ResetShortcut", resetLabel);

            UI::Panels::EndSettingsRow(row);
        }
    }

    ImGui::EndDisabled();
}

void MockToolWindow::DrawPositionPolicy(MiniSettings &settings)
{
    auto        &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float  gap      = m3Styles.GetPixels(M3Spec::List::segmentedGap);
    const float  rowH     = UI::Panels::ButtonHeight();
    const float  lineGap  = m3Styles.GetPixels(M3Spec::dp<8>());
    const float  padX     = m3Styles.GetPixels(M3Spec::List::paddingX);

    const std::array<std::string_view, 3> labels{"基于鼠标位置", "基于插入符位置", "无"};
    constexpr std::array<int, 3>          values{1, 2, 0};
    std::vector<float> widths;
    widths.reserve(labels.size());
    for (const auto label : labels)
    {
        widths.push_back(UI::Panels::MeasureButton(label, {}, M3Spec::SizeTips::XSMALL, M3Spec::ButtonShape::Square));
    }
    const float contentW = ImGui::GetContentRegionAvail().x - padX * 2.0F;
    const auto  flow     = UI::Panels::FlowLayoutPositions({}, contentW, gap, lineGap, rowH, widths);

    const auto row = UI::Panels::BeginPlainSettingsRow({}, {}, 0.0F, flow.height);
    if (row)
    {
        for (size_t i = 0; i < labels.size(); ++i)
        {
            ImGui::SetCursorScreenPos({row.contentX + flow.positions[i].x, row.contentY + flow.positions[i].y});
            ImGuiEx::M3::ButtonConfiguration config;
            config.Tonal().Square();
            config.toggle   = true;
            config.selected = settings.posUpdatePolicy == values[i];
            config.Size(M3Spec::SizeTips::XSMALL);
            if (ImGuiEx::M3::Button(labels[i], config))
            {
                settings.posUpdatePolicy = values[i];
            }
        }

        UI::Panels::EndSettingsRow(row);
    }
}

MiniSettings g_settings{};
MiniState    g_state{};
float        g_scrollTo = -1.F; // --scroll=N: scroll the panel child once for screenshots
float        g_windowWidth = 0.F; // --width=N: first-use window width override (0 = default)
bool         g_verticalPreview = false; // --vertical: preset the candidate orientation

// Bridged from DesignPreviewMain's --tab= argument.
mock::Menu g_initialTab = mock::Menu::InputStatus;

} // namespace mock

void DrawMockToolWindow()
{
    static mock::MockToolWindow window = [] {
        mock::g_settings.verticalCandidateList = mock::g_verticalPreview;
        // Stress mode also binds a three-modifier chord so the shortcut row's
        // stacked fallback (keycaps + links that no longer share one line)
        // renders alongside the doubled labels.
        if (std::getenv("PREVIEW_LANG_STRESS") != nullptr)
        {
            mock::g_settings.shortcut = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_F2;
        }
        mock::MockToolWindow w;
        w.m_menu = mock::g_initialTab;
        return w;
    }();
    window.Draw(mock::g_settings, mock::g_state);
}

// PREVIEW_LANGBAR=1: the floating language bar instead of the settings window.
// PREVIEW_LANGBAR_LEGACY=1 draws the pre-redesign sequence (no divider, boxed
// menu item, 12dp gaps) for before/after comparison.
void DrawMockLanguageBar()
{
    static bool        pinned   = true;
    const std::string  language = "中文(简体)";
    const std::string  desc     = "微信输入法";
    const std::string  mode     = "英";
    const bool         legacy   = std::getenv("PREVIEW_LANGBAR_LEGACY") != nullptr;

    auto flags = ImGuiEx::WindowFlags().AlwaysAutoResize().NoNav().NoDecoration();
    if (ImGuiEx::M3::BeginFloatingToolbar("##MockLanguageBar", nullptr, M3Spec::ToolBarColors::Standard, flags))
    {
        constexpr auto iconButtonColors = ImGuiEx::M3::Spec::IconButtonColors::Standard;
        if (ImGuiEx::M3::SmallIconButton(pinned ? static_cast<std::string_view>(ICON_PIN_OFF) : ICON_PIN, iconButtonColors))
        {
            pinned = !pinned;
        }
        ImGui::SameLine();
        (void)ImGuiEx::M3::SmallIconButton(ICON_SETTINGS, iconButtonColors);

        if (legacy)
        {
            ImGui::SameLine();
            ImGuiEx::M3::AlignedLabel(mode);
            ImGuiEx::M3::SameLine(0.F, M3Spec::StandardSmallButtonGroup::BetweenSpace);
            ImGuiEx::M3::MenuItemConfiguration config{};
            config.supportingText = desc;
            (void)ImGuiEx::M3::MenuItem(language, false, config);
        }
        else
        {
            auto      &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
            const float barH    = m3Styles.GetPixels(M3Spec::ToolBarSizing<M3Spec::ToolBarVariant::Floating>::HorizontalContainerHeight);
            const float ruleH   = m3Styles.GetPixels(M3Spec::dp<24>());
            const float gap     = m3Styles.GetPixels(M3Spec::dp<14>());
            ImGui::SameLine(0.0F, gap);
            const auto lineTop = ImGui::GetCursorScreenPos().y;
            const float ruleX  = ImGui::GetCursorScreenPos().x;
            ImGui::GetWindowDrawList()->AddLine(
                {ruleX, lineTop + (barH - ruleH) * 0.5F},
                {ruleX, lineTop + (barH + ruleH) * 0.5F},
                ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::outlineVariant]),
                1.0F
            );
            ImGui::SameLine(0.0F, gap * 2.0F + 1.0F);

            ImGuiEx::M3::AlignedLabel(mode);
            ImGuiEx::M3::SameLine(0.F, M3Spec::dp<8>());
            ImGuiEx::M3::MenuItemConfiguration config{};
            config.supportingText       = desc;
            config.leadingSpace         = 4.0F;
            config.trailingSpace        = 8.0F;
            config.transparentContainer = true;
            (void)ImGuiEx::M3::MenuItem(language, false, config);
        }

        ImGuiEx::M3::EndFloatingToolbar();
    }
}
