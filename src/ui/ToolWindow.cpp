//
// Created by jamie on 2026/3/10.
//

#include "ui/ToolWindow.h"

#include "hooks/MeridianBridge.h"
#include "hooks/NirnLabBridge.h"
#include "hooks/PrismaBridge.h"
#include "hooks/SkseMenuFrameworkBridge.h"
#include "i18n/translator_manager.h"
#include "icons.h"
#include "ime/ImeController.h"
#include "imguiex/ErrorNotifier.h"
#include "imguiex/imguiex_enum_wrap.h"
#include "imguiex/imguiex_m3.h"
#include "imguiex/m3/spec/appbar.h"
#include "imguiex/m3/spec/layout.h"
#include "imguiex/m3/spec/others.h"
#include "log.h"
#include "menu/MenuNames.h"
#include "path_utils.h"
#include "ui/panels/PanelWidgets.h"
#include "ui/SettingsManager.h"
#include "utils/Utils.h"
#include "WCharUtils.h"

#include <SKSE/Interfaces.h>
#include <SKSE/Logger.h>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>

namespace Ime::Global
{
extern std::string g_skseVersion; ///< defined in plugin.cpp, set at PluginLoad
} // namespace Ime::Global

namespace Ime::UI
{

ToolWindow::ToolWindow()
{
    Skyrim::ShowMenu(ToolWindowMenuName);
}

ToolWindow::~ToolWindow()
{
    Skyrim::HideMenu(ToolWindowMenuName);
}

//! Window header: "SimpleIME / 设置" on the left, a quiet X icon button on the
//! right, hairline rule underneath — the settings shell's only chrome.
void ToolWindow::DrawSettingsHeader(Settings &settings)
{
    auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float headerH  = m3Styles.GetPixels(M3Spec::SmallAppbar::ContainerHeight);
    const float padX     = m3Styles.GetPixels(M3Spec::dp<20>());
    const auto  origin   = ImGui::GetCursorScreenPos();
    const float availW   = ImGui::GetContentRegionAvail().x;

    {
        const auto  fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::TitleLarge>();
        const auto &typeScale = m3Styles.GetLastText().currText;
        const std::string title = std::string("SimpleIME / ") + std::string(Translate("Settings.Settings"));
        ImGui::SetCursorScreenPos({origin.x + padX, origin.y + (headerH - typeScale.lineHeight) * 0.5F});
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::TitleLarge>(title, M3Spec::ColorRole::onSurface);
    }

    const auto  closeSizing = M3Spec::GetIconButtonSizing(M3Spec::SizeTips::SMALL, M3Spec::IconButtonWidths::Default);
    const float closeExtent =
        std::max(m3Styles.GetPixels(closeSizing.containerHeight), m3Styles.GetPixels(M3Spec::IconButtonCommon::MinLayoutSize));
    ImGui::SetCursorScreenPos({origin.x + availW - padX - closeExtent, origin.y + (headerH - closeExtent) * 0.5F});
    if (ImGuiEx::M3::SmallIconButton(ICON_X, ImGuiEx::M3::Spec::IconButtonColors::Standard))
    {
        CancelShortcutCapture();
        settings.runtimeData.toolWindowShowing = false;
    }
    ImGuiEx::M3::SetItemToolTip(Translate("Settings.Close"));

    ImGui::SetCursorScreenPos({origin.x, origin.y + headerH});
    ImGuiEx::M3::Divider();
}

//! Text-only navigation inside a bordered card: the selected entry gets a
//! tonal pill and a small square indicator, everything else stays plain.
void ToolWindow::DrawSidebar()
{
    auto      &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float width   = m3Styles.GetPixels(M3Spec::dp<200>());
    const float pad     = m3Styles.GetPixels(M3Spec::dp<8>());
    const auto  guard   = ImGuiEx::StyleGuard()
                            .Color<ImGuiCol_ChildBg>(m3Styles.Colors()[M3Spec::ColorRole::surfaceContainerLowest])
                            .Color<ImGuiCol_Border>(m3Styles.Colors()[M3Spec::ColorRole::outlineVariant])
                            .Style<ImGuiStyleVar_WindowPadding>(ImVec2(pad, pad))
                            .Style<ImGuiStyleVar_ItemSpacing>(ImVec2(0.0F, m3Styles.GetPixels(M3Spec::dp<2>())))
                            .Style<ImGuiStyleVar_ChildBorderSize>(1.0F);
    if (!ImGui::BeginChild("##SettingsSidebar", {width, 0.0F}, ImGuiEx::ChildFlags().Borders()))
    {
        ImGui::EndChild();
        return;
    }

    const auto selectMenu = [this](const Menu menu) {
        if (menu != m_currentMenu)
        {
            CancelShortcutCapture();
            m_currentMenu = menu;
        }
    };

    const float itemH = m3Styles.GetPixels(M3Spec::dp<40>());
    const auto  item  = [&](const char *strId, const std::string_view label, const Menu menu) {
        const bool selected = m_currentMenu == menu;
        const auto pos      = ImGui::GetCursorScreenPos();
        const float w       = ImGui::GetContentRegionAvail().x;
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
            drawList->AddRectFilled(bb.Min, bb.Max, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[M3Spec::ColorRole::secondaryContainer]), m3Styles.GetPixels(M3Spec::ShapeCorner::Medium));
        }
        else if (hovered || held)
        {
            Panels::DrawStateWash(drawList, bb, held, M3Spec::ColorRole::surfaceContainerHighest, M3Spec::ColorRole::onSurface, m3Styles.GetPixels(M3Spec::ShapeCorner::Medium));
        }

        float textX = bb.Min.x + m3Styles.GetPixels(M3Spec::dp<12>());
        if (selected)
        {
            const float square   = m3Styles.GetPixels(M3Spec::dp<10>());
            const float centerY  = bb.GetCenter().y;
            const float squareY  = centerY - square * 0.5F;
            drawList->AddRectFilled(
                {bb.Min.x + m3Styles.GetPixels(M3Spec::dp<12>()), squareY},
                {bb.Min.x + m3Styles.GetPixels(M3Spec::dp<12>()) + square, squareY + square},
                ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[M3Spec::ColorRole::primary]),
                m3Styles.GetPixels(M3Spec::dp<4>())
            );
            textX += square + m3Styles.GetPixels(M3Spec::dp<8>());
        }

        {
            const auto  fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
            const auto  color    = selected ? M3Spec::ColorRole::onSecondaryContainer : (hovered ? M3Spec::ColorRole::onSurface : M3Spec::ColorRole::onSurfaceVariant);
            // The rail is a fixed-width surface: long translations elide with
            // an ellipsis instead of clipping mid-glyph at the border, and the
            // tooltip discloses the full label.
            const float maxTextW = bb.Max.x - textX - m3Styles.GetPixels(M3Spec::dp<12>());
            const auto  elided   = Panels::ElideText(label, maxTextW);
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

    item("##NavInputStatus", Translate("Settings.Sidebar.InputStatus"), Menu::InputStatus);
    item("##NavDisplay", Translate("Settings.Sidebar.Display"), Menu::Display);
    item("##NavFontBuilder", Translate("Settings.Sidebar.FontBuilder"), Menu::FontBuilder);
    item("##NavAdvanced", Translate("Settings.Sidebar.Advanced"), Menu::Advanced);

    ImGui::EndChild();
}

void ToolWindow::Draw(Settings &settings)
{
    constexpr ImVec2 CENTER_ALIGN_PIVOT(0.5F, 0.5F);
    constexpr float  DEFAULT_WINDOW_HEIGHT_FACTOR = 0.82F;

    const auto &viewport  = ImGui::GetMainViewport();
    auto       &m3Styles  = ImGuiEx::M3::Context::GetM3Styles();
    const float margin    = m3Styles.GetPixels(M3Spec::Layout::Compact::Margin);
    const float maxWidth  = std::max(1.0F, std::min(m3Styles.GetPixels(M3Spec::Layout::Large::Breakpoint), viewport->Size.x - margin * 2.0F));
    const float minWidth  = std::min(m3Styles.GetPixels(M3Spec::dp<508>()), maxWidth);
    const float maxHeight = std::max(1.0F, viewport->Size.y - margin * 2.0F);
    const float minHeight = std::min(m3Styles.GetPixels(M3Spec::dp<360>()), maxHeight);
    const float defaultWindowWidth  = std::clamp(m3Styles.GetPixels(1024.0F), minWidth, maxWidth);
    const float defaultWindowHeight = std::clamp(viewport->Size.y * DEFAULT_WINDOW_HEIGHT_FACTOR, minHeight, maxHeight);

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
                    switch (m_currentMenu)
                    {
                        case Menu::InputStatus: return Translate("Settings.Sidebar.InputStatus");
                        case Menu::Display: return Translate("Settings.Sidebar.Display");
                        case Menu::FontBuilder: return Translate("Settings.Sidebar.FontBuilder");
                        case Menu::Advanced: return Translate("Settings.Sidebar.Advanced");
                    }
                    return Translate("Settings.Sidebar.InputStatus");
                };
                if (ImGuiEx::M3::BeginCombo("##SettingsNavigation", currentLabel()))
                {
                    const auto menuItem = [&](const std::string_view label, const Menu menu) {
                        if (ImGuiEx::M3::MenuItem(label, m_currentMenu == menu))
                        {
                            if (menu != m_currentMenu)
                            {
                                CancelShortcutCapture();
                                m_currentMenu = menu;
                            }
                        }
                    };
                    menuItem(Translate("Settings.Sidebar.InputStatus"), Menu::InputStatus);
                    menuItem(Translate("Settings.Sidebar.Display"), Menu::Display);
                    menuItem(Translate("Settings.Sidebar.FontBuilder"), Menu::FontBuilder);
                    menuItem(Translate("Settings.Sidebar.Advanced"), Menu::Advanced);
                    ImGuiEx::M3::EndCombo();
                }
                ImGuiEx::M3::Divider();
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
                switch (m_currentMenu)
                {
                    case Menu::Display:
                        DrawMenuAppearance(settings);
                        break;
                    case Menu::FontBuilder:
                        DrawMenuFontBuilder(settings);
                        break;
                    case Menu::InputStatus:
                        DrawMenuInputStatus(settings);
                        break;
                    case Menu::Advanced:
                        DrawMenuAdvanced(settings);
                        break;
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImeController::GetInstance()->SyncImeStateIfDirty();
}

void ToolWindow::DrawMenuAppearance(Settings &settings)
{
    m_panelAppearance.Draw(settings);

    // sync theme config
    const auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const auto &[contrastLevel, sourceColor, darkMode, variant] = m3Styles.Colors().GetSchemeConfig();
    settings.appearance.schemeConfig.sourceColor       = sourceColor;
    settings.appearance.schemeConfig.darkMode          = darkMode;
    settings.appearance.schemeConfig.contrastLevel     = contrastLevel;
    settings.appearance.schemeConfig.variant           = variant;
}

void ToolWindow::DrawMenuFontBuilder(Settings &settings)
{
    m_fontBuilderView.Draw(m_fontBuilder, settings);
}

namespace
{
//! Mods held right now, read through the ImGuiMod_* aliases — the exact values
//! ImeMenu feeds io.AddKeyEvent with (GFx distinguishes neither left nor right).
auto CurrentModifierChord() -> ImGuiKeyChord
{
    ImGuiKeyChord mods = 0;
    if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) mods |= ImGuiMod_Ctrl;
    if (ImGui::IsKeyDown(ImGuiMod_Shift)) mods |= ImGuiMod_Shift;
    if (ImGui::IsKeyDown(ImGuiMod_Alt)) mods |= ImGuiMod_Alt;
    if (ImGui::IsKeyDown(ImGuiMod_Super)) mods |= ImGuiMod_Super;
    return mods;
}

//! Scan the named keys for the chord being captured: the first non-modifier key
//! that is down completes the chord. Escape is reserved (it closes the settings
//! window) and mouse buttons are excluded (clicking the capture UI itself must
//! not bind a mouse button). Returns 0 while only modifiers are held.
auto DetectCapturedShortcutChord() -> ImGuiKeyChord
{
    const ImGuiKeyChord mods = CurrentModifierChord();
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key = static_cast<ImGuiKey>(key + 1))
    {
        if (!ImGui::IsKeyDown(key)) continue;
        switch (key)
        {
            case ImGuiKey_Escape:
            case ImGuiKey_LeftCtrl:
            case ImGuiKey_RightCtrl:
            case ImGuiKey_LeftShift:
            case ImGuiKey_RightShift:
            case ImGuiKey_LeftAlt:
            case ImGuiKey_RightAlt:
            case ImGuiKey_LeftSuper:
            case ImGuiKey_RightSuper:
            // Aliases sharing key storage with the modifiers above: with Ctrl held
            // these report down too and must not become the chord's main key.
            case ImGuiKey_ReservedForModCtrl:
            case ImGuiKey_ReservedForModShift:
            case ImGuiKey_ReservedForModAlt:
            case ImGuiKey_ReservedForModSuper:
            case ImGuiKey_MouseLeft:
            case ImGuiKey_MouseRight:
            case ImGuiKey_MouseMiddle:
            case ImGuiKey_MouseX1:
            case ImGuiKey_MouseX2:
            case ImGuiKey_MouseWheelX:
            case ImGuiKey_MouseWheelY:
                continue;
            default:
                return mods | key;
        }
    }
    return 0;
}

//! True while ANY key of the chord (a bound modifier or the main key) is down.
//! Used to keep the post-capture swallow armed until the modifiers themselves
//! are released — releasing only the main key while still holding Ctrl must not
//! re-enable the chord, or a re-press of the main key would toggle the overlay.
auto IsAnyChordKeyHeld(ImGuiKeyChord chord) -> bool
{
    if ((chord & ImGuiMod_Ctrl) != 0 && ImGui::IsKeyDown(ImGuiMod_Ctrl)) return true;
    if ((chord & ImGuiMod_Shift) != 0 && ImGui::IsKeyDown(ImGuiMod_Shift)) return true;
    if ((chord & ImGuiMod_Alt) != 0 && ImGui::IsKeyDown(ImGuiMod_Alt)) return true;
    if ((chord & ImGuiMod_Super) != 0 && ImGui::IsKeyDown(ImGuiMod_Super)) return true;
    const auto mainKey = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    return mainKey != ImGuiKey_None && ImGui::IsKeyDown(mainKey);
}

//! Readable measure for row supporting copy: long descriptions wrap here
//! instead of running the full width of the window.
auto SupportingMeasure() -> float
{
    return ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::dp<500>());
}

//! The SKSE log directory (cached: fixed once the plugin DLL path is known).
//! Empty when SKSE cannot resolve its log directory.
auto ResolveLogDir() -> std::filesystem::path
{
    static const std::filesystem::path cached = []() -> std::filesystem::path {
        const auto logDir = SKSE::log::log_directory();
        return logDir ? *logDir : std::filesystem::path{};
    }();
    return cached;
}

//! Resolves `path` to the physical location Explorer can reach. Mod managers
//! virtualize Data\ paths inside the game process (the file really lives in
//! the mod folder), but explorer.exe runs outside that VFS, so spawning it
//! with the virtual path fails with "location unavailable". Opening the path
//! here goes through the VFS and GetFinalPathNameByHandleW reads the real
//! path back off the resulting kernel handle. Unvirtualized installs resolve
//! to themselves; if the path can't be opened the input is returned as-is.
auto ResolveExplorerPath(const std::filesystem::path &path) -> std::filesystem::path
{
    // FILE_FLAG_BACKUP_SEMANTICS alone opens both files and directories; a
    // zero access mask keeps it a pure query, no privileges required.
    const HANDLE handle = CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return path;
    }
    std::wstring resolved;
    for (DWORD size = 0;;)
    {
        resolved.resize(size == 0 ? MAX_PATH : size);
        size = GetFinalPathNameByHandleW(handle, resolved.data(), static_cast<DWORD>(resolved.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (size == 0)
        {
            resolved.clear();
            break;
        }
        if (size < resolved.size())
        {
            resolved.resize(size);
            break;
        }
    }
    CloseHandle(handle);
    if (resolved.empty())
    {
        return path;
    }
    // Strip \\?\ (\\?\UNC\ → \\) so explorer /select accepts the path.
    if (resolved.starts_with(LR"(\\?\UNC\)"))
    {
        resolved = LR"(\\)" + resolved.substr(8);
    }
    else if (resolved.starts_with(LR"(\\?\)"))
    {
        resolved.erase(0, 4);
    }
    return { resolved.begin(), resolved.end() };
}

//! Opens `target` in Explorer. With selectFile, highlights the file inside its
//! parent folder (explorer /select) — used for the config file and the log
//! file; otherwise the folder itself opens. The target is first resolved to
//! its physical path (mod-manager VFS). Fire-and-forget: if the file to
//! select can't be confirmed to exist, its parent folder opens instead of
//! letting Explorer error on a missing /select target.
void OpenInExplorer(const std::filesystem::path &target, const bool selectFile)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(target, ec);
    if (ec)
    {
        return;
    }
    const std::filesystem::path physical = ResolveExplorerPath(absolute);
    if (selectFile)
    {
        std::error_code existsEc;
        if (std::filesystem::exists(physical, existsEc) && !existsEc)
        {
            const std::wstring params = std::format(L"/select,\"{}\"", physical.wstring());
            ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        }
        else
        {
            // File gone (e.g. the config was never shipped): settle for the
            // parent folder instead of Explorer erroring on a missing target.
            ShellExecuteW(nullptr, L"open", physical.parent_path().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    else
    {
        ShellExecuteW(nullptr, L"open", physical.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

//! Trailing caption + tone for a compatibility toggle, from the bridge's
//! install-time outcome and the switch's current value. The bridges latch
//! their config gate when they install (game thread, SKSE messaging), so a
//! fresh flip cannot move the hooks: the caption says when a restart applies
//! it instead of echoing the switch.
auto BridgeStatusCaption(const bool configOn, const Hooks::SupportState state) -> std::pair<std::string_view, M3Spec::ColorRole>
{
    using S  = Hooks::SupportState;
    using CR = M3Spec::ColorRole;
    if (state == S::Active)
    {
        return configOn ? std::pair{Translate("Settings.Compat.Active"), CR::primary}
                        : std::pair{Translate("Settings.Compat.RestartDisable"), CR::onSurfaceVariant};
    }
    if (state == S::Pending)
    {
        return {Translate("Settings.Compat.Pending"), CR::onSurfaceVariant};
    }
    if (state == S::Off)
    {
        return {configOn ? Translate("Settings.Compat.RestartEnable") : Translate("Settings.Compat.Off"), CR::onSurfaceVariant};
    }
    if (state == S::NotDetected)
    {
        return {Translate("Settings.Compat.NotDetected"), CR::onSurfaceVariant};
    }
    if (state == S::Standoff)
    {
        return {Translate("Settings.Compat.Standoff"), CR::error};
    }
    return {Translate("Settings.Compat.Unavailable"), CR::error}; // Failed
}

//! One compatibility row: description + switch like any toggle, plus a
//! trailing caption reporting the bridge's install-time outcome. A non-empty
//! detailTooltip recolors the caption as a warning and spells the detail out
//! on hover (the Meridian row folds two backends into one state).
void DrawCompatibilityRow(
    const char                *strId,
    bool                      &value,
    const std::string_view     title,
    const std::string_view     supporting,
    const Hooks::SupportState  state,
    const std::string_view     detailTooltip = {}
)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const auto [caption, captionColor] = BridgeStatusCaption(value, state);
    const bool warned = !detailTooltip.empty();
    const auto captionTone = warned ? M3Spec::ColorRole::error : captionColor;

    float captionW = 0.0F;
    {
        const auto fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
        captionW = ImGui::CalcTextSize(caption.data(), caption.data() + caption.size()).x;
    }
    const float trailing = captionW + m3Styles.GetPixels(M3Spec::dp<12>()) + Panels::SwitchReserve();
    const auto  row      = Panels::BeginSettingsRow(strId, title, supporting, trailing, m3Styles.GetPixels(M3Spec::dp<24>()), SupportingMeasure());
    if (row)
    {
        Panels::RowTitle(row);
        Panels::RowSupporting(row, supporting);
        Panels::RowTrailingText(row, caption, captionTone, Panels::SwitchReserve());
        Panels::RowTrailingSwitch(row, value);
        Panels::EndSettingsRow(row);
    }
    if (row.pressed)
    {
        value = !value;
    }
    if (warned)
    {
        ImGuiEx::M3::SetItemToolTip(detailTooltip);
    }
}

//! Stable English token for a bridge state in the diagnostics clipboard text
//! (bug reports are read back by the maintainer, not by the UI).
auto BridgeStateToken(const Hooks::SupportState state) -> std::string_view
{
    switch (state)
    {
        case Hooks::SupportState::Active: return "active";
        case Hooks::SupportState::Standoff: return "conflict";
        case Hooks::SupportState::NotDetected: return "not detected";
        case Hooks::SupportState::Failed: return "unavailable";
        case Hooks::SupportState::Off: return "off";
        case Hooks::SupportState::Pending: return "pending";
    }
    return "unknown";
}

//! The Meridian row folds the UIPlatform and View/1 backends into one state
//! (Active when either is live); when a backend failed or stands off next to
//! a live one, the tooltip spells out both tokens so the split is visible.
auto MeridianDetailTooltip(
    const Hooks::SupportState uiPlatform,
    const Hooks::SupportState view
) -> std::string
{
    using S = Hooks::SupportState;
    const bool warning = uiPlatform == S::Failed || uiPlatform == S::Standoff || view == S::Failed || view == S::Standoff;
    if (!warning || uiPlatform == view)
    {
        return {};
    }
    return std::format("UIPlatform: {}, View/1: {}", BridgeStateToken(uiPlatform), BridgeStateToken(view));
}

//! The SKSE log file, `<log dir>\<plugin>.log` (cached: the location is fixed
//! once the plugin DLL path is known). Empty when SKSE cannot resolve its log
//! directory.
auto ResolveLogFile() -> std::filesystem::path
{
    static const std::filesystem::path cached = []() -> std::filesystem::path {
        const auto logDir = SKSE::log::log_directory();
        if (!logDir)
        {
            return {};
        }
        std::filesystem::path file = *logDir;
        file /= SKSE::PluginDeclaration::GetSingleton()->GetName();
        file += L".log";
        return file;
    }();
    return cached;
}

//! The SKSE log file's path as UTF-8 for the clipboard. Empty when SKSE
//! cannot resolve its log directory.
auto ResolveLogFilePath() -> std::string
{
    static const std::string cached = WCharUtils::ToString(ResolveLogFile().wstring());
    return cached;
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

//! The bound chord rendered as physical keycaps: "Ctrl + Shift + F2".
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
            ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::LabelMedium>("+", M3Spec::ColorRole::onSurfaceVariant);
            ImGui::SetCursorScreenPos({ImGui::GetItemRectMax().x + spacing, baseY});
        }
        Panels::Keycap(label);
        first = false;
    });
}
} // namespace

//! Live IME state as a tinted headline card: the two status entries and the
//! focus action flow left to right on as many lines as the window width
//! allows — long translations wrap to a next line instead of the fixed
//! thirds colliding. This is what people check when something "swallows
//! keys", so it leads the panel.
void ToolWindow::DrawStatusCard(const Settings &settings) const
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    if (const auto card = Panels::BeginSettingsCard("##StatusCard", M3Spec::ColorRole::primaryContainer, false); card)
    {
        const auto &state = Core::State::GetInstance();
        const bool  imeOn = state.NotHas(Core::State::IME_DISABLED);
        const bool  focus = state.Has(Core::State::TEXT_SERVICE_FOCUS);

        const float  padX   = m3Styles.GetPixels(M3Spec::List::paddingX);
        const float  padY   = m3Styles.GetPixels(M3Spec::dp<12>());
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;

        ImGui::SetCursorScreenPos({origin.x + padX, origin.y + padY});
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::TitleSmall>(Translate("Settings.Behaviour.LiveStatus"), M3Spec::ColorRole::onPrimaryContainer);
        ImGui::Dummy({0.0F, m3Styles.GetPixels(M3Spec::dp<8>())});

        const auto &onLabel  = Translate("Settings.Behaviour.StatusModOn");
        const auto &offLabel = Translate("Settings.Behaviour.StatusModOff");
        const auto  modLabel = settings.enableMod ? onLabel : offLabel;
        const auto  imeLabel = imeOn ? Translate("Settings.Behaviour.StatusImeOn") : Translate("Settings.Behaviour.StatusImeOff");
        const std::string_view focusLabel = Translate("Settings.Behaviour.ForceFocusIme");

        // Measure every entry before the row's extent commits: the flow
        // layout needs the widths to decide where the lines break.
        float dotW  = 0.0F;
        float modW  = 0.0F;
        float imeW  = 0.0F;
        float linkW = 0.0F;
        float linkH = 0.0F;
        {
            const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
            dotW  = ImGui::GetTextLineHeight() * 0.6F; // StatusDot's diameter at this font
            modW  = ImGui::CalcTextSize(modLabel.data(), modLabel.data() + modLabel.size()).x;
            imeW  = ImGui::CalcTextSize(imeLabel.data(), imeLabel.data() + imeLabel.size()).x;
            linkW = Panels::TextLinkWidth(focusLabel);
            linkH = Panels::TextLinkHeight();
        }
        const float dotGap  = m3Styles.GetPixels(M3Spec::dp<8>());
        const float chipGap = m3Styles.GetPixels(M3Spec::dp<16>());
        const float lineGap = m3Styles.GetPixels(M3Spec::dp<8>());
        const float chipH   = m3Styles.GetPixels(M3Spec::dp<32>());
        const float lineH   = std::max(chipH, linkH);
        const float contentW = width - padX * 2.0F;
        const float contentTop = ImGui::GetCursorScreenPos().y;
        const auto flow = Panels::FlowLayoutPositions(
            {origin.x + padX, contentTop},
            contentW,
            chipGap,
            lineGap,
            lineH,
            {dotW + dotGap + modW, dotW + dotGap + imeW, linkW}
        );

        const auto chip = [&](const ImVec2 &position, const bool on, const std::string_view label, const std::string_view tooltip) {
            const auto  fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
            const auto &typeScale = m3Styles.GetLastText().currText;
            ImGui::SetCursorScreenPos({position.x, position.y + (chipH - typeScale.lineHeight) * 0.5F});
            Panels::StatusDot(on);
            ImGui::SameLine(0.0F, dotGap);
            ImGuiEx::M3::AlignedLabel(label, M3Spec::ColorRole::onPrimaryContainer);
            if (!tooltip.empty())
            {
                ImGuiEx::M3::SetItemToolTip(tooltip);
            }
        };

        chip(flow.positions[0], settings.enableMod, modLabel, {});
        chip(flow.positions[1], imeOn, imeLabel, Translate("Settings.Behaviour.ImeEnabledTooltip"));

        ImGui::SetCursorScreenPos({flow.positions[2].x, flow.positions[2].y + (lineH - linkH) * 0.5F});
        ImGui::BeginDisabled(focus);
        if (Panels::TextLink("##ForceFocusIme", focusLabel, M3Spec::ColorRole::onPrimaryContainer, M3Spec::ColorRole::primaryContainer))
        {
            ImeController::GetInstance()->ForceFocusIme();
        }
        ImGui::EndDisabled();
        ImGuiEx::M3::SetItemToolTip(Translate("Settings.Behaviour.FocusTooltip"));

        ImGui::SetCursorScreenPos({origin.x, contentTop + flow.height});
        ImGui::Dummy({0.0F, padY});
    }
    Panels::EndSettingsCard();
}

void ToolWindow::DrawMenuInputStatus(Settings &settings)
{
    // The page shares the window surface; only the cards carry their own fill.
    const auto pageGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(ImGuiEx::M3::Context::GetM3Styles().Colors()[M3Spec::ColorRole::surface]);
    if (ImGui::BeginChild("InputStatus", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding().AutoResizeY()))
    {
        Panels::PageHeader(Translate("Settings.Sidebar.InputStatus"), Translate("Settings.Page.InputStatus.Support"));
        DrawStatusCard(settings);

        Panels::SectionHeader(Translate("Settings.Behaviour.Input"));
        if (Panels::BeginSettingsCard("##InputCard"))
        {
            // The description is the visible documentation of the master switch,
            // wrapped at a fixed measure so lines stay scannable.
            bool enableMod = settings.enableMod;
            if (Panels::SettingsToggleRow(
                    "##EnableMod", Translate("Settings.Behaviour.EnableMod"), Translate("Settings.Behaviour.EnableModToolTip"), enableMod, SupportingMeasure()
                ))
            {
                ImeController::GetInstance()->EnableMod(enableMod);
            }

            // The three dependent switches collapse into one quiet summary row.
            // Each entry is a status dot + text link: the dot carries the state
            // in shape as well as color (filled accent vs. hollow ring), and
            // the run wraps to another line when the width runs out.
            ImGui::BeginDisabled(!settings.enableMod);
            ImGuiEx::M3::Divider();
            {
                auto    &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
                const float dotGap  = m3Styles.GetPixels(M3Spec::dp<6>());
                const float itemGap = m3Styles.GetPixels(M3Spec::dp<16>());
                const float lineGap = m3Styles.GetPixels(M3Spec::dp<4>());
                const float linkH   = Panels::TextLinkHeight();
                const float padX    = m3Styles.GetPixels(M3Spec::List::paddingX);

                // Measure the whole run first: the row height must commit with
                // the wrapped line count already known.
                float          dotW    = 0.0F;
                std::array<float, 3> itemW{};
                {
                    const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
                    dotW = ImGui::GetTextLineHeight() * 0.6F;
                    itemW[0] = dotW + dotGap + Panels::TextLinkWidth(Translate("Settings.Behaviour.ShortAutoToggleKeyboard"));
                    itemW[1] = dotW + dotGap + Panels::TextLinkWidth(Translate("Settings.Behaviour.ShortUnicodePaste"));
                    itemW[2] = dotW + dotGap + Panels::TextLinkWidth(Translate("Settings.Behaviour.KeepImeOpen"));
                }
                const float contentW = ImGui::GetContentRegionAvail().x - padX * 2.0F;
                const auto flow = Panels::FlowLayoutPositions({}, contentW, itemGap, lineGap, linkH, {itemW.begin(), itemW.end()});

                if (const auto row = Panels::BeginPlainSettingsRow({}, {}, 0.0F, flow.height); row)
                {
                    float x = 0.0F;
                    const auto item = [&](const size_t index, const char *strId, const std::string_view label, bool &value, const std::string_view tooltip) {
                        x = row.contentX + flow.positions[index].x;
                        const float y = row.contentY + flow.positions[index].y;
                        {
                            const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
                            ImGui::SetCursorScreenPos({x, y + (linkH - ImGui::GetTextLineHeight()) * 0.5F});
                            Panels::StatusDot(value);
                        }
                        x += dotW + dotGap;
                        ImGui::SetCursorScreenPos({x, y});
                        if (Panels::TextLink(
                                strId, label, value ? M3Spec::ColorRole::onSurface : M3Spec::ColorRole::onSurfaceVariant
                            ))
                        {
                            value = !value;
                        }
                        ImGuiEx::M3::SetItemToolTip(tooltip);
                    };
                    item(
                        0,
                        "##AutoToggleKeyboard",
                        Translate("Settings.Behaviour.ShortAutoToggleKeyboard"),
                        settings.autoToggleKeyboard,
                        Translate("Settings.Behaviour.AutoToggleKeyboardToolTip")
                    );
                    item(
                        1,
                        "##UnicodePaste",
                        Translate("Settings.Behaviour.ShortUnicodePaste"),
                        settings.input.enableUnicodePaste,
                        Translate("Settings.Behaviour.EnableUnicodePasteToolTip")
                    );
                    bool keepImeOpen = settings.input.keepImeOpen;
                    item(2, "##KeepImeOpen", Translate("Settings.Behaviour.KeepImeOpen"), keepImeOpen, Translate("Settings.Behaviour.KeepImeOpenTooltip"));
                    if (keepImeOpen != settings.input.keepImeOpen)
                    {
                        settings.input.keepImeOpen = keepImeOpen;
                        ImeController::GetInstance()->MarkDirty();
                    }
                    Panels::EndSettingsRow(row);
                }
            }
            ImGui::EndDisabled();
        }
        Panels::EndSettingsCard();

        Panels::SectionHeader(Translate("Settings.Behaviour.Shortcut"));
        if (Panels::BeginSettingsCard("##ShortcutCard"))
        {
            DrawShortcutSection(settings);
        }
        Panels::EndSettingsCard();

        // The input-bridge compatibility switches were TOML-only until now.
        // Their descriptions are the TOML comments verbatim; the trailing
        // caption reports what actually happened at install time (see
        // BridgeStatusCaption) — including "not detected", which explains why
        // a switch can be on while the feature is silently inactive.
        Panels::SectionHeader(Translate("Settings.Behaviour.Compatibility"));
        if (Panels::BeginSettingsCard("##CompatibilityCard"))
        {
            DrawCompatibilityRow(
                "##MeridianSupport",
                settings.input.meridianSupport,
                Translate("Settings.Behaviour.MeridianSupport"),
                Translate("Settings.Behaviour.MeridianSupportToolTip"),
                Hooks::MeridianBridge::State(),
                MeridianDetailTooltip(Hooks::NirnLabBridge::State(), Hooks::MeridianBridge::ViewBackendState())
            );
            Panels::RowDivider();
            DrawCompatibilityRow(
                "##PrismaAvoidance",
                settings.input.prismaAvoidance,
                Translate("Settings.Behaviour.PrismaAvoidance"),
                Translate("Settings.Behaviour.PrismaAvoidanceToolTip"),
                Hooks::PrismaBridge::State()
            );
            Panels::RowDivider();
            DrawCompatibilityRow(
                "##SkseMenuFrameworkSupport",
                settings.input.skseMenuFrameworkSupport,
                Translate("Settings.Behaviour.SkseMenuFrameworkSupport"),
                Translate("Settings.Behaviour.SkseMenuFrameworkSupportToolTip"),
                Hooks::SkseMenuFrameworkBridge::State()
            );
        }
        Panels::EndSettingsCard();
    }
    ImGui::EndChild();
}

void ToolWindow::DrawMenuAdvanced(Settings &settings)
{
    const auto pageGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(ImGuiEx::M3::Context::GetM3Styles().Colors()[M3Spec::ColorRole::surface]);
    if (ImGui::BeginChild("Advanced", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding().AutoResizeY()))
    {
        Panels::PageHeader(Translate("Settings.Sidebar.Advanced"), Translate("Settings.Page.Advanced.Support"));
        Panels::SectionHeader(Translate("Settings.Advanced.Troubleshooting"));
        if (Panels::BeginSettingsCard("##TroubleshootingCard"))
        {
            ImGui::BeginDisabled(!settings.enableMod);
            (void)Panels::SettingsToggleRow(
                "##FixInconsistent",
                Translate("Settings.Behaviour.FixInconsistentTextEntryCount"),
                Translate("Settings.Behaviour.FixInconsistentTextEntryCountToolTip"),
                settings.fixInconsistentTextEntryCount,
                SupportingMeasure()
            );
            ImGui::EndDisabled();

            ImGuiEx::M3::Divider();
            DrawLogPathRow();

            ImGuiEx::M3::Divider();
            DrawDiagnosticsRow(settings);
        }
        Panels::EndSettingsCard();

        Panels::SectionHeader(Translate("Settings.Advanced.Logging"));
        if (Panels::BeginSettingsCard("##LoggingCard"))
        {
            DrawLogLevelRow(settings);
            ImGuiEx::M3::Divider();
            DrawErrorDurationRow(settings);
        }
        Panels::EndSettingsCard();

        Panels::SectionHeader(Translate("Settings.Advanced.Environment"));
        if (Panels::BeginSettingsCard("##EnvironmentCard"))
        {
            DrawEnvironmentRows(settings);
        }
        Panels::EndSettingsCard();

        Panels::SectionHeader(Translate("Settings.Advanced.ConfigTitle"));
        if (Panels::BeginSettingsCard("##ConfigOnlyCard"))
        {
            const std::string_view openLabel = Translate("Settings.Open");
            const float            openW     = Panels::TextLinkWidth(openLabel);
            const float            linkH     = Panels::TextLinkHeight();
            const auto             row       = Panels::BeginPlainSettingsRow(
                Translate("Settings.Advanced.ConfigPath"), Translate("Settings.Advanced.ConfigDescription"), openW, linkH, SupportingMeasure());
            if (row)
            {
                Panels::RowTitle(row);
                Panels::RowSupporting(row, Translate("Settings.Advanced.ConfigDescription"));
                if (Panels::RowTrailingTextLink(row, "##OpenConfigDir", openLabel))
                {
                    OpenInExplorer(SettingsManager::ConfigFilePath(), true);
                }
                Panels::EndSettingsRow(row);
            }

            ImGuiEx::M3::Divider();
            DrawConfigStatusRow();
        }
        Panels::EndSettingsCard();
    }
    ImGui::EndChild();
}

//! "复制日志路径" row: puts the SKSE log file's location (UTF-8) on the
//! clipboard, with a sibling link that locates the log file in Explorer; the
//! trailing copy link flips to a brief acknowledgment after a copy.
void ToolWindow::DrawLogPathRow()
{
    const std::string_view copyLabel   = Translate("Settings.Advanced.CopyLogPath");
    const std::string_view copiedLabel = Translate("Settings.Advanced.Copied");
    const std::string_view openLabel   = Translate("Settings.Open");
    const float            linkH      = Panels::TextLinkHeight();
    const float            linkGap    = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::dp<8>());
    const float            copyW      = Panels::TextLinkWidth(copyLabel);
    const float            copiedW    = Panels::TextLinkWidth(copiedLabel);
    const float            openW      = Panels::TextLinkWidth(openLabel);
    const bool             copied     = m_logCopiedAt >= 0.0F && ImGui::GetTime() - m_logCopiedAt < 1.5;
    const float            reserveW   = std::max(copyW, copiedW) + linkGap + openW;

    if (const auto row = Panels::BeginPlainSettingsRow(Translate("Settings.Advanced.LogFile"), {}, reserveW, linkH); row)
    {
        Panels::RowTitle(row);
        ImGui::SetCursorScreenPos({row.trailingRight - reserveW, row.centerY - linkH * 0.5F});
        if (Panels::TextLink("##CopyLogPath", copied ? copiedLabel : copyLabel))
        {
            const std::string logPath = ResolveLogFilePath();
            if (!logPath.empty())
            {
                ImGui::SetClipboardText(logPath.c_str());
                m_logCopiedAt = static_cast<float>(ImGui::GetTime());
            }
        }
        if (const std::string logPath = ResolveLogFilePath(); !logPath.empty())
        {
            ImGuiEx::M3::SetItemToolTip(logPath.c_str());
        }

        if (Panels::RowTrailingTextLink(row, "##OpenLogFile", openLabel))
        {
            const std::filesystem::path logFile = ResolveLogFile();
            if (!logFile.empty())
            {
                OpenInExplorer(logFile, true);
            }
            else
            {
                OpenInExplorer(ResolveLogDir(), false);
            }
        }
        Panels::EndSettingsRow(row);
    }
}

//! "配置读取状态" row: validates the on-disk configuration once per window
//! open and reports the outcome. A parse failure also surfaces its raw
//! message as the supporting copy, so a broken user file stops being an
//! invisible startup fallback to defaults.
void ToolWindow::DrawConfigStatusRow()
{
    if (!m_configStatus)
    {
        m_configStatus = ConfigSerializer::ValidateConfiguration(SettingsManager::ConfigFilePath());
    }

    const auto       &status = *m_configStatus;
    std::string_view  label;
    M3Spec::ColorRole labelColor = M3Spec::ColorRole::onSurfaceVariant;
    switch (status.kind)
    {
        case ConfigSerializer::ConfigStatusKind::Ok:
            label      = Translate("Settings.Advanced.ConfigStatusOk");
            labelColor = status.ignoredKeys.empty() ? M3Spec::ColorRole::primary : M3Spec::ColorRole::error;
            break;
        case ConfigSerializer::ConfigStatusKind::NotFound:
            label = Translate("Settings.Advanced.ConfigStatusMissing");
            break;
        case ConfigSerializer::ConfigStatusKind::ParseError:
            label      = Translate("Settings.Advanced.ConfigStatusError");
            labelColor = M3Spec::ColorRole::error;
            break;
    }

    // Mistyped keys parse fine but were dropped in favor of the defaults at
    // load time; list them so the divergence from the on-disk file is visible.
    std::string ignoredDetail;
    if (status.kind == ConfigSerializer::ConfigStatusKind::Ok && !status.ignoredKeys.empty())
    {
        ignoredDetail = std::format("{} config key(s) ignored, unexpected type:", status.ignoredKeys.size());
        for (const auto &key : status.ignoredKeys)
        {
            ignoredDetail += std::format(" {}", key);
        }
    }
    const std::string_view supporting =
        status.kind == ConfigSerializer::ConfigStatusKind::ParseError ? std::string_view{status.detail}
                                                                      : std::string_view{ignoredDetail};

    // Reserve the trailing label's width so the title wraps short of it
    // instead of colliding under long translations, and measure the row with
    // supporting copy only when one is actually rendered (a parse error's
    // detail); Ok/NotFound would otherwise reserve an invisible extra line.
    float statusW = 0.0F;
    {
        auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
        const auto  fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
        statusW = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + m3Styles.GetPixels(M3Spec::dp<12>());
    }
    const auto row = Panels::BeginPlainSettingsRow(
        Translate("Settings.Advanced.ConfigStatus"),
        supporting,
        statusW,
        0.0F,
        SupportingMeasure()
    );
    if (row)
    {
        Panels::RowTitle(row);
        if (!supporting.empty())
        {
            Panels::RowSupporting(row, supporting);
        }
        Panels::RowTrailingText(row, label, labelColor);
        Panels::EndSettingsRow(row);
    }
}

//! "复制诊断信息" row: one click puts a support-ready summary (versions,
//! environment, bridge outcomes, config status, paths) on the clipboard, so a
//! bug report starts with the facts debugging needs instead of a Q&A round.
void ToolWindow::DrawDiagnosticsRow(const Settings &settings)
{
    if (!m_configStatus)
    {
        // Same lazy validation as DrawConfigStatusRow; either row may run first.
        m_configStatus = ConfigSerializer::ValidateConfiguration(SettingsManager::ConfigFilePath());
    }

    const std::string_view copyLabel   = Translate("Settings.Copy");
    const std::string_view copiedLabel = Translate("Settings.Advanced.Copied");
    const float            linkH      = Panels::TextLinkHeight();
    const float            copyW      = Panels::TextLinkWidth(copyLabel);
    const float            copiedW    = Panels::TextLinkWidth(copiedLabel);
    const bool             copied     = m_diagCopiedAt >= 0.0F && ImGui::GetTime() - m_diagCopiedAt < 1.5;
    const float            reserveW   = std::max(copyW, copiedW);

    if (const auto row = Panels::BeginPlainSettingsRow(
            Translate("Settings.Advanced.Diagnostics"), Translate("Settings.Advanced.DiagnosticsSupport"), reserveW, linkH, SupportingMeasure());
        row)
    {
        Panels::RowTitle(row);
        Panels::RowSupporting(row, Translate("Settings.Advanced.DiagnosticsSupport"));
        ImGui::SetCursorScreenPos({row.trailingRight - reserveW, row.centerY - linkH * 0.5F});
        if (Panels::TextLink("##CopyDiagnostics", copied ? copiedLabel : copyLabel))
        {
            const std::string diagnostics = BuildDiagnosticsText(settings);
            ImGui::SetClipboardText(diagnostics.c_str());
            m_diagCopiedAt = static_cast<float>(ImGui::GetTime());
        }
        Panels::EndSettingsRow(row);
    }
}

//! The diagnostics clipboard text. Fixed English labels: this is read back in
//! bug reports (often cross-language), not rendered as UI copy.
std::string ToolWindow::BuildDiagnosticsText(const Settings &settings) const
{
    const auto *plugin = SKSE::PluginDeclaration::GetSingleton();
    std::string text   = std::format("SimpleIME {}\n", plugin->GetVersion().string("."));
    text += std::format("SKSE {}\n", Ime::Global::g_skseVersion);
    text += std::format("DPI awareness: {}\n", settings.forceDpiAwareness ? "enabled" : "disabled");
    text += std::format("IME framework: {}\n", settings.enableTsf ? "TSF" : "Imm32");
    text += std::format("Meridian UI: {}\n", BridgeStateToken(Hooks::MeridianBridge::State()));
    text += std::format("NirnLab UIPlatform: {} {}\n", Hooks::NirnLabBridge::VersionDescription(),
                        BridgeStateToken(Hooks::NirnLabBridge::State()));
    text += std::format("Prisma UI: {}\n", BridgeStateToken(Hooks::PrismaBridge::State()));
    text += std::format("SKSE Menu Framework: {}\n", BridgeStateToken(Hooks::SkseMenuFrameworkBridge::State()));

    std::string configLine = "unknown";
    if (m_configStatus)
    {
        switch (m_configStatus->kind)
        {
            case ConfigSerializer::ConfigStatusKind::Ok:
                configLine = "Ok";
                break;
            case ConfigSerializer::ConfigStatusKind::NotFound:
                configLine = "not found";
                break;
            case ConfigSerializer::ConfigStatusKind::ParseError:
                configLine = std::format("parse error: {}", m_configStatus->detail);
                break;
        }
    }
    text += std::format("Config: {}\n", configLine);
    text += std::format("Config file: {}\n", WCharUtils::ToString(SettingsManager::ConfigFilePath().wstring()));
    text += std::format("Log file: {}", ResolveLogFilePath());
    return text;
}

//! "日志级别" row: the dropdown hot-applies through spdlog's registry (the file
//! logger was registered as the default logger at startup), so a deeper level
//! starts producing lines without a restart.
void ToolWindow::DrawLogLevelRow(Settings &settings)
{
    struct LevelName
    {
        spdlog::level::level_enum level;
        std::string_view          name;
    };
    static constexpr std::array<LevelName, 7> kLevels{{
        {spdlog::level::trace,    "Trace"   },
        {spdlog::level::debug,    "Debug"   },
        {spdlog::level::info,     "Info"    },
        {spdlog::level::warn,     "Warn"    },
        {spdlog::level::err,      "Error"   },
        {spdlog::level::critical, "Critical"},
        {spdlog::level::off,      "Off"     },
    }};
    const auto current = std::find_if(kLevels.begin(), kLevels.end(), [&](const LevelName &entry) { return entry.level == settings.logging.level; });
    const std::string preview = current != kLevels.end() ? std::string(current->name) : std::string("Info");

    const float padX   = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::List::paddingX);
    const float comboW = Panels::RowTrailingComboWidth(preview, ImGui::GetContentRegionAvail().x - padX * 2.0F);
    const auto  row    = Panels::BeginPlainSettingsRow(
        Translate("Settings.Advanced.LogLevel"), Translate("Settings.Advanced.LogLevelSupport"), comboW, Panels::ComboButtonHeight(), SupportingMeasure());
    if (!row)
    {
        return;
    }
    Panels::RowTitle(row);
    Panels::RowSupporting(row, Translate("Settings.Advanced.LogLevelSupport"));

    const bool open = Panels::BeginRowTrailingCombo(row, "##LogLevelCombo", preview);
    if (open)
    {
        for (const auto &[level, name] : kLevels)
        {
            const bool selected = level == settings.logging.level;
            if (ImGuiEx::M3::MenuItem(name, selected) && !selected)
            {
                settings.logging.level = level;
                spdlog::set_level(level);
            }
        }
    }
    Panels::EndRowTrailingCombo(open);
    Panels::EndSettingsRow(row);
}

//! "错误提示时长" row: presets cover the practical range (TOML still accepts
//! arbitrary values); -1 keeps an error on screen until dismissed.
void ToolWindow::DrawErrorDurationRow(Settings &settings)
{
    static constexpr std::array<std::int32_t, 5> kDurations{-1, 5, 10, 30, 60};

    const std::string preview = settings.appearance.errorDisplayDuration < 0
                                    ? std::string(Translate("Settings.Advanced.ErrorDurationNever"))
                                    : std::format("{} s", settings.appearance.errorDisplayDuration);
    const float padX   = ImGuiEx::M3::Context::GetM3Styles().GetPixels(M3Spec::List::paddingX);
    const float comboW = Panels::RowTrailingComboWidth(preview, ImGui::GetContentRegionAvail().x - padX * 2.0F);
    const auto  row    = Panels::BeginPlainSettingsRow(
        Translate("Settings.Advanced.ErrorDuration"), Translate("Settings.Advanced.ErrorDurationSupport"), comboW, Panels::ComboButtonHeight(), SupportingMeasure());
    if (!row)
    {
        return;
    }
    Panels::RowTitle(row);
    Panels::RowSupporting(row, Translate("Settings.Advanced.ErrorDurationSupport"));

    const bool open = Panels::BeginRowTrailingCombo(row, "##ErrorDurationCombo", preview);
    if (open)
    {
        for (const std::int32_t duration : kDurations)
        {
            const bool selected = duration == settings.appearance.errorDisplayDuration;
            const std::string label =
                duration < 0 ? std::string(Translate("Settings.Advanced.ErrorDurationNever")) : std::format("{} s", duration);
            if (ImGuiEx::M3::MenuItem(label, selected) && !selected)
            {
                settings.appearance.errorDisplayDuration = duration;
                ErrorNotifier::GetInstance().SetMessageDuration(duration);
            }
        }
    }
    Panels::EndRowTrailingCombo(open);
    Panels::EndSettingsRow(row);
}

//! Two read-only status rows: process DPI awareness and the IME framework.
//! Both are consumed once at plugin load, so a switch would only pretend to
//! work — the honest presentation is the current value.
void ToolWindow::DrawEnvironmentRows(const Settings &settings)
{
    const auto drawValueRow = [](const std::string_view title, const std::string_view supporting, const std::string_view value) {
        float valueW = 0.0F;
        {
            auto &m3Styles       = ImGuiEx::M3::Context::GetM3Styles();
            const auto fontScope = m3Styles.UseTextRole<M3Spec::List::textRole>();
            valueW = ImGui::CalcTextSize(value.data(), value.data() + value.size()).x + m3Styles.GetPixels(M3Spec::dp<12>());
        }
        if (const auto row = Panels::BeginPlainSettingsRow(title, supporting, valueW, 0.0F, SupportingMeasure()); row)
        {
            Panels::RowTitle(row);
            Panels::RowSupporting(row, supporting);
            Panels::RowTrailingText(row, value, M3Spec::ColorRole::onSurfaceVariant);
            Panels::EndSettingsRow(row);
        }
    };

    drawValueRow(
        Translate("Settings.Advanced.DpiAwareness"),
        Translate("Settings.Advanced.DpiAwarenessSupport"),
        settings.forceDpiAwareness ? Translate("Settings.Advanced.StateEnabled") : Translate("Settings.Advanced.StateDisabled")
    );
    ImGuiEx::M3::Divider();
    drawValueRow(
        Translate("Settings.Advanced.TsfFramework"),
        Translate("Settings.Advanced.TsfFrameworkSupport"),
        settings.enableTsf ? std::string_view{"TSF"} : std::string_view{"Imm32"}
    );
}

void ToolWindow::DrawShortcutSection(Settings &settings)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    // Disarming while the mod is off: the capture would otherwise complete from
    // raw key scans even with its button disabled.
    if (!settings.enableMod)
    {
        m_capturingShortcut = false;
    }
    ImGui::BeginDisabled(!settings.enableMod);

    const float linkHeight    = Panels::TextLinkHeight();
    const float linkGap       = m3Styles.GetPixels(M3Spec::dp<4>());

    if (m_capturingShortcut)
    {
        const std::string_view cancelLabel = Translate("Settings.Behaviour.CancelShortcutCapture");
        const float            cancelWidth = Panels::TextLinkWidth(cancelLabel);
        const auto             row         = Panels::BeginPlainSettingsRow({}, {}, cancelWidth, linkHeight);
        if (row)
        {
            ImGuiEx::M3::AlignedLabel(Translate("Settings.Behaviour.CapturingShortcut"), M3Spec::ColorRole::primary);
            if (Panels::RowTrailingTextLink(row, "##CancelShortcutCapture", cancelLabel))
            {
                m_capturingShortcut = false;
            }
            Panels::EndSettingsRow(row);
        }

        if (const ImGuiKeyChord chord = DetectCapturedShortcutChord(); chord != 0)
        {
            settings.shortcut            = chord;
            m_capturingShortcut          = false;
            m_capturedChordAwaitRelease  = true; // swallow the chord until the user lets go of it
        }
    }
    else
    {
        const std::string_view changeLabel = Translate("Settings.Behaviour.ChangeShortcut");
        const std::string_view resetLabel  = Translate("Settings.Behaviour.ResetShortcut");
        const float            changeWidth = Panels::TextLinkWidth(changeLabel);
        const float            resetWidth  = Panels::TextLinkWidth(resetLabel);
        const float            keycapsHeight = ImGui::GetFrameHeight();
        const float            linksReserve  = changeWidth + linkGap + resetWidth;

        // Measure the keycap run (keycaps separated by gap + "+" + gap) so a
        // wide chord can be told apart from the trailing links before the row
        // commits: the two share the line while they fit, and the links drop
        // to a second line when long translations plus a long chord collide.
        const float keycapSpacing = m3Styles.GetPixels(M3Spec::dp<8>());
        float       keycapsWidth  = 0.0F;
        {
            const auto  fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelMedium>();
            const float plusWidth = ImGui::CalcTextSize("+").x;
            bool        firstKey  = true;
            ForEachShortcutKeycap(settings.shortcut, [&](const std::string_view keyLabel) {
                keycapsWidth += (firstKey ? 0.0F : keycapSpacing * 2.0F + plusWidth) + Panels::KeycapWidth(keyLabel);
                firstKey = false;
            });
        }

        const float padX     = m3Styles.GetPixels(M3Spec::List::paddingX);
        const float stackGap = m3Styles.GetPixels(M3Spec::dp<8>());
        const bool  stacked  = keycapsWidth + keycapSpacing + linksReserve > ImGui::GetContentRegionAvail().x - padX * 2.0F;
        const float  rowControlHeight = stacked ? keycapsHeight + stackGap + linkHeight : std::max(linkHeight, keycapsHeight);
        const auto   row              = Panels::BeginPlainSettingsRow({}, {}, linksReserve, rowControlHeight);
        if (row)
        {
            const float keycapsY = stacked ? row.contentY : row.centerY - keycapsHeight * 0.5F;
            const float linkY    = stacked ? row.contentY + keycapsHeight + stackGap : row.centerY - linkHeight * 0.5F;

            ImGui::SetCursorScreenPos({row.contentX, keycapsY});
            DrawShortcutKeycaps(settings.shortcut);

            ImGui::SetCursorScreenPos({row.trailingRight - linksReserve, linkY});
            if (Panels::TextLink("##ChangeShortcut", changeLabel))
            {
                m_capturingShortcut = true;
            }
            ImGuiEx::M3::SetItemToolTip(Translate("Settings.Behaviour.ChangeShortcutTooltip"));

            ImGui::SetCursorScreenPos({row.trailingRight - resetWidth, linkY});
            if (Panels::TextLink("##ResetShortcut", resetLabel))
            {
                settings.shortcut = ImGuiKey_F2;
            }
            ImGuiEx::M3::SetItemToolTip(Translate("Settings.Behaviour.ResetShortcutTooltip"));

            Panels::EndSettingsRow(row);
        }
    }

    ImGui::EndDisabled();

    // Arm the swallow latch consumed at the top of ImeWnd::Draw next frame. While
    // armed, a press of the currently-bound chord must not also toggle the overlay
    // through the global chord evaluation. Beyond the capture itself, the latch
    // stays armed until every key of the just-captured chord (modifiers included)
    // has been released: the capture press's keys are down in ImGui, and a re-press
    // of the main key before letting go of the modifiers would otherwise toggle the
    // overlay out from under the settings window. Triggering is edge-based, so this
    // never suppresses a genuine fresh press — the press frame sees a disarmed
    // latch unless some chord key was already held in the previous frame.
    const bool anyChordKeyHeld = IsAnyChordKeyHeld(settings.shortcut);
    if (m_capturedChordAwaitRelease && !anyChordKeyHeld)
    {
        m_capturedChordAwaitRelease = false; // everything released — the new binding goes live
    }
    settings.runtimeData.swallowShortcutToggle = m_capturingShortcut || (m_capturedChordAwaitRelease && anyChordKeyHeld);
}

} // namespace Ime::UI
