//
// Created by jamie on 2026/1/15.
//
#define IMGUI_DEFINE_MATH_OPERATORS

#include "i18n/Translator.h"
#include "i18n/translator_manager.h"
#include "icons.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imguiex/ImGuiEx.h"
#include "imguiex/Material3.h"
#include "imguiex/imguiex_enum_wrap.h"
#include "imguiex/imguiex_m3.h"
#include "imguiex/m3/spec/layout.h"
#include "ui/fonts/FontBuilder.h"
#include "ui/fonts/FontBuilderPanel.h"
#include "ui/fonts/ImFontWrap.h"
#include "ui/fonts/preview_panel.h"
#include "ui/panels/PanelWidgets.h"

#include <algorithm>
#include <array>

namespace Ime
{
namespace
{
//! Merge the source font into the target font.
//! The source font will be removed after merging, and the target font will be updated with the merged font.
//! The source font should be committable and only contains a single font source, otherwise the merge will fail.
auto MergeFont(ImFontWrap &target, ImFontWrap &source) -> bool
{
    if (!source.IsCommittableSingleFont())
    {
        return false;
    }

    auto *imFont                     = source.TakeFont();
    auto *fontConfig                 = imFont->Sources[0];
    fontConfig->FontDataOwnedByAtlas = false; // avoid copy font data

    ImFontConfig config;
    ImStrncpy(config.Name, fontConfig->Name, IM_COUNTOF(config.Name));
    // Keep ImFontConfig defaults; see ImGuiEx::AddFonts — the FreeType
    // rasterizer ignores the legacy Oversample/PixelSnapH fields.
    config.MergeMode    = true;
    config.DstFont      = target.UnsafeGetFont();
    config.FontData     = fontConfig->FontData;
    config.FontDataSize = fontConfig->FontDataSize;

    ImGui::GetIO().Fonts->AddFont(&config);
    ImGui::GetIO().Fonts->RemoveFont(imFont);

    target.AddFontInfo(source.GetFontNameOr(0), source.GetFontPathOr(0));
    return true;
}

void DrawHelpModal()
{
    if (auto dialog = ImGuiEx::M3::DialogModal(Translate("Settings.FontBuilder.HelpTitle")); dialog)
    {
        dialog.SupportingText(Translate("Settings.FontBuilder.Help1"), true);
        dialog.SupportingText(Translate("Settings.FontBuilder.Help2"), true);
        dialog.ActionButton(Translate("Settings.Apply"));
    }
}

void DrawWarningsModal()
{
    if (auto dialog = ImGuiEx::M3::DialogModal(Translate("Settings.FontBuilder.WarningTitle")); dialog)
    {
        dialog.SupportingText(Translate("Settings.FontBuilder.Warning1"), true);
        dialog.ActionButton(Translate("Settings.Apply"));
    }
}

} // namespace

ImFontWrap::ImFontWrap(ImFont *imFont, std::string_view fontName, std::string_view fontPath, bool a_owner)
    : font(imFont), owner(a_owner && font != nullptr)
{
    m_fontNames.emplace_back(fontName);
    m_fontPathList.emplace_back(fontPath);
}

auto ImFontWrap::operator=(const ImFontWrap &other) -> ImFontWrap &
{
    if (this == &other) return *this;
    RemoveFontIfOwned();

    font           = other.font;
    owner          = false;
    m_fontNames    = other.m_fontNames;
    m_fontPathList = other.m_fontPathList;
    return *this;
}

auto ImFontWrap::operator=(ImFontWrap &&other) noexcept -> ImFontWrap &
{
    if (this == &other) return *this;
    RemoveFontIfOwned();

    font           = other.font;
    owner          = other.owner;
    m_fontNames    = std::move(other.m_fontNames);
    m_fontPathList = std::move(other.m_fontPathList);

    other.font  = nullptr;
    other.owner = false;
    return *this;
}

auto ImFontWrap::IsCommittable() const -> bool
{
    return owner && font != nullptr;
}

auto ImFontWrap::IsCommittableSingleFont() const -> bool
{
    return owner && font != nullptr && font->Sources.size() == 1;
}

void ImFontWrap::Cleanup()
{
    RemoveFontIfOwned();
    font  = nullptr;
    owner = false;
    m_fontNames.clear();
    m_fontPathList.clear();
}

void ImFontWrap::RemoveFontIfOwned() const
{
    if (owner && font != nullptr && ImGui::GetCurrentContext() != nullptr)
    {
        ImGui::GetIO().Fonts->RemoveFont(font);
    }
}

//! Add a font to the builder.
//! The font will be invalid after adding.
auto FontBuilder::AddFont(int fontId, ImFontWrap &imFont) -> bool
{
    bool result = true;

    if (std::ranges::contains(m_usedFontIds, fontId)) // already used
    {
        return false;
    }

    if (!IsBuilding())
    {
        m_baseFont = std::move(imFont);
    }
    else // merge to base font
    {
        result = MergeFont(m_baseFont, imFont);
    }
    if (result)
    {
        m_usedFontIds.push_back(fontId);
    }
    return result;
}

auto FontBuilder::ApplyFont(Settings &settings) -> bool
{
    if (!m_baseFont.IsCommittable())
    {
        return false;
    }

    auto &io = ImGui::GetIO();
    if (auto *imFont = m_baseFont.TakeFont(); io.FontDefault != imFont)
    {
        io.Fonts->RemoveFont(io.FontDefault);
        io.FontDefault = imFont;
    }
    settings.resources.fontPathList = std::move(m_baseFont.GetFontPathList());
    Reset();
    return true;
}

void UI::FontBuilderPanel::Draw(FontBuilder &fontBuilder, Settings &settings)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    const auto styleGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(m3Styles.Colors()[M3Spec::ColorRole::surface]);
    if (ImGui::BeginChild("FontBuilderPage", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding()))
    {
        // Page-wide support actions ride the header's right edge; the content
        // below keeps only build actions.
        const std::array headerLinks{
            UI::Panels::PageHeaderLink{"##FontBuilderWarning", Translate("Settings.FontBuilder.Warning")},
            UI::Panels::PageHeaderLink{"##FontBuilderHelp", Translate("Settings.FontBuilder.Help")},
        };
        int linkPressed = -1;
        UI::Panels::PageHeader(Translate("Settings.Sidebar.FontBuilder"), Translate("Settings.Page.Fonts.Support"), headerLinks, &linkPressed);
        if (linkPressed == 0)
        {
            ImGui::OpenPopup(TITLE_WARNING);
        }
        else if (linkPressed == 1)
        {
            ImGui::OpenPopup(Translate("Settings.FontBuilder.HelpTitle").data());
        }

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float  margin    = m3Styles.GetPixels(M3Spec::Layout::Compact::Margin);
        // The font picker is low-density content: master–detail from the M3
        // medium class up (the spec allows two panes for settings-like
        // surfaces), so the default settings window fills both columns instead
        // of stacking a short list over a dead zone.
        if (available.x >= m3Styles.GetPixels(M3Spec::Layout::Medium::Breakpoint))
        {
            const float listWidth = available.x >= m3Styles.GetPixels(M3Spec::Layout::Expanded::Breakpoint)
                                        ? std::min(m3Styles.GetPixels(M3Spec::Layout::Expanded::FixedPaneWidth), available.x * 0.40F)
                                        : available.x * 0.42F;
            m_PreviewPanel.Draw(fontBuilder, {listWidth, available.y});
            ImGui::SameLine(0.0F, margin);
            m_PreviewPanel.DrawResultCard(fontBuilder, settings, m3Styles, {available.x - listWidth - margin, available.y});
        }
        else
        {
            const float listHeight = std::clamp(available.y * 0.38F, m3Styles.GetPixels(M3Spec::dp<140>()), m3Styles.GetPixels(M3Spec::dp<320>()));
            m_PreviewPanel.Draw(fontBuilder, {available.x, listHeight});
            ImGui::Dummy({0.0F, margin});
            m_PreviewPanel.DrawResultCard(fontBuilder, settings, m3Styles, {available.x, available.y - listHeight - margin});
        }

        DrawModals();
    }
    ImGui::EndChild();
}

void UI::FontBuilderPanel::DrawModals()
{
    DrawHelpModal();
    DrawWarningsModal();
}

} // namespace Ime
