//
// Created by jamie on 2026/2/8.
//
#define IMGUI_DEFINE_MATH_OPERATORS

#include "ui/fonts/preview_panel.h"

#include <algorithm>
#include <format>
#include <string>

#include "i18n/Translator.h"
#include "i18n/translator_manager.h"
#include "icons.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imguiex/ImGuiEx.h"
#include "imguiex/Material3.h"
#include "imguiex/imguiex_enum_wrap.h"
#include "imguiex/imguiex_m3.h"
#include "imguiex/m3/facade/base.h"
#include "imguiex/m3/spec/icon_button.h"
#include "imguiex/m3/spec/layout.h"
#include "ui/fonts/FontManager.h"
#include "ui/fonts/ImFontWrap.h"
#include "ui/panels/PanelWidgets.h"

namespace Ime::UI
{
// ReSharper disable All
static constexpr auto PREVIEW_TEXT = {
    R"(!@#$%^&*()_+-=[]{}|;':",.<>?/)",
    R"(-- Unicode & Fallback --)",
    R"(Latín: áéíóú ñ  |  FullWidth: ＡＢＣ１２３)",
    R"(CJK: 繁體中文测试 / 简体中文测试 / 日本語 / 한국어)",
    R"(-- Emoji & Variation --)",
    R"(Icons: 🥰💀✌︎🌴🐢🐐🍄🍻👑📸😬👀🚨🏡)",
    R"(New: 🐦‍🔥 🍋‍🟩 🍄‍🟫 🙂‍↕️ 🙂‍↔️)",
    R"(-- Skyrim Immersion --)",
    R"(Dovah: Dovahkiin, naal ok zin los vahriin!)",
    R"("I used to be an adventurer like you...")",
};

// ReSharper restore All

namespace
{

struct StatusBar
{
    std::string_view      icon;
    std::string           text;
    M3Spec::ColorRole     role = M3Spec::ColorRole::onSurfaceVariant;
    bool                  tooltipPath = false;
};

/**
 * @param selectedIndex current selected FontInfo index. will be set if select a new.
 * @return is select a new row.
 */
auto FontsTable(FontInfo::Index &selectedIndex, const std::vector<FontInfo> &fontInfos) -> bool
{
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(fontInfos.size()));
    bool selectedNew = false;
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
        {
            const auto &fontInfo = fontInfos[static_cast<size_t>(row)];
            ImGui::PushID(row);

            const bool selected = selectedIndex == fontInfo.GetIndex();
            const bool clicked  = ImGuiEx::M3::MenuItem(fontInfo.GetName(), selected);
            if (clicked && !selected)
            {
                selectedNew   = true;
                selectedIndex = fontInfo.GetIndex();
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
    }

    return selectedNew;
}

} // namespace

//! Submit the specimen block: the preview text lines rendered in the font
//! currently being previewed (the builder font when previewing the result).
void FontPreviewPanel::DrawSpecimen()
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    ImGui::PushFont(m_imFont.UnsafeGetFont(), 0);
    const float wrapWidth = ImGui::GetContentRegionAvail().x;
    for (const auto &text : PREVIEW_TEXT)
    {
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::BodyLarge>(text, ImGuiEx::M3::Spec::ColorRole::onSurface, wrapWidth);
    }
    ImGui::PopFont();
}

void FontPreviewPanel::DrawSearchBar(const std::vector<FontInfo> &fontInfos)
{
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    const bool edited = ImGuiEx::M3::SearchBar(
        "Filter", m_textFilter.InputBuf, IM_COUNTOF(m_textFilter.InputBuf), {.icon = ICON_SEARCH, .hintText = Translate("Settings.FontBuilder.Search")}
    );
    ImGui::PopItemFlag();
    if (edited)
    {
        m_searchDebounceTimer.Poke();
    }

    if (m_searchDebounceTimer.Check())
    {
        m_textFilter.Build();
        UpdateDisplayFontInfos(fontInfos);
    }
}

void FontPreviewPanel::DrawFontsList(const std::vector<FontInfo> &fontInfos)
{
    // Empty filtered list means "no match" — falling back to the full list
    // (the old behavior) made the search look broken. m_filterActive
    // distinguishes "no filter" from "filter matched nothing".
    const auto &displayFontInfos = m_filterActive ? m_displayFontInfos : fontInfos;
    if (FontsTable(m_interactState.selectedIndex, displayFontInfos))
    {
        m_previewDebounceTimer.Poke();
        m_state = State::DEBOUNCING;
    }

    if (m_previewDebounceTimer.Check())
    {
        m_state = State::EMPTY;
        // selectedIndex stores the DWrite system-font-set index (see
        // FontsTable), NOT a position in fontInfos — FindInstalledFonts skips
        // fonts without a usable name, so the two domains diverge and the old
        // vector indexing previewed/added the wrong font.
        const auto it = std::ranges::find_if(
            fontInfos, [idx = m_interactState.selectedIndex](const FontInfo &info) { return info.GetIndex() == idx; });
        if (it == fontInfos.end())
        {
            m_previewDebounceTimer.Reset();
        }
        else
        {
            const auto &fontInfo = *it;
            const auto  filePath = GetFontFilePath(fontInfo);
            m_imFont.Cleanup();
            if (!filePath.empty() && PreviewFont(fontInfo.GetName(), filePath))
            {
                m_state = State::PREVIEWING;
            }
            else
            {
                m_state = State::NOT_SUPPORTED_FONTS;
            }
        }
    }
}

//! Left picker pane: the search bar stays pinned while the list scrolls, so a
//! full-height list never carries the filter out of view.
void FontPreviewPanel::Draw(FontBuilder &fontBuilder, const ImVec2 &size)
{
    if (ImGui::BeginChild("FontsView", size, ImGuiEx::ChildFlags()))
    {
        DrawSearchBar(fontBuilder.GetFontManager().GetFontInfoList());
        if (ImGui::BeginChild("##FontsList", {0.0F, 0.0F}, ImGuiEx::ChildFlags()))
        {
            DrawFontsList(fontBuilder.GetFontManager().GetFontInfoList());
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

//! Right result card sized to `size`: status strip / scrolling specimen /
//! build queue / actions. Every fixed block is measured before the card child
//! commits, so the specimen takes exactly the height the rest leaves over and
//! the card fills its pane without any dead zone below.
void FontPreviewPanel::DrawResultCard(FontBuilder &fontBuilder, Settings &settings, ImGuiEx::M3::M3Styles &m3Styles, const ImVec2 &size)
{
    if (size.x <= 0.0F || size.y <= 0.0F)
    {
        return;
    }

    const float padX = m3Styles.GetPixels(M3Spec::List::paddingX);
    const float padY = m3Styles.GetPixels(M3Spec::List::paddingY);
    const float gap  = m3Styles.GetPixels(M3Spec::dp<8>());
    const float contentW = std::max(size.x - padX * 2.0F, 0.0F);

    // ---- fixed block measurements (the card height commits up front) ----
    // Icon buttons submit a square hit box: max(containerHeight, 48dp layout min).
    const auto iconSizing = ImGuiEx::M3::Spec::GetIconButtonSizing(ImGuiEx::M3::Spec::SizeTips::XSMALL, ImGuiEx::M3::Spec::IconButtonWidths::Default);
    const float iconBtnH  = std::max(m3Styles.GetPixels(iconSizing.containerHeight), m3Styles.GetPixels(ImGuiEx::M3::Spec::IconButtonCommon::MinLayoutSize));
    const float iconBtnW  = iconBtnH;

    float statusLineH  = 0.0F;
    float queueTitleH  = 0.0F;
    float listLineH    = 0.0F;
    float queueEmptyH  = 0.0F;
    {
        const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::BodySmall>();
        statusLineH = m3Styles.GetLastText().currText.lineHeight;
        queueEmptyH = ImGuiEx::M3::MeasureWrappedText(Translate("Settings.FontBuilder.BuildQueueEmpty"), contentW).y;
    }
    {
        const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::TitleSmall>();
        queueTitleH = m3Styles.GetLastText().currText.lineHeight;
    }
    {
        const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::List::textRole>();
        listLineH = m3Styles.GetLastText().currText.lineHeight;
    }

    const float statusH       = padY * 2.0F + statusLineH;
    const float queueHeaderH  = std::max(queueTitleH, iconBtnH);
    const float queueTitleGap = m3Styles.GetPixels(M3Spec::dp<4>());
    const float queueTopGap   = m3Styles.GetPixels(M3Spec::dp<8>());
    const float queueBotGap   = m3Styles.GetPixels(M3Spec::dp<12>());

    const auto  &builtNames = fontBuilder.GetBaseFont().GetFontNames();
    constexpr int kMaxVisibleRows = 4;
    const int     rowCount        = static_cast<int>(builtNames.size());
    const float   rowH            = listLineH + m3Styles.GetPixels(M3Spec::dp<8>());
    const float   queueContentH   = rowCount > 0 ? std::min(rowCount, kMaxVisibleRows) * rowH : queueEmptyH;
    const float   queueH          = queueTopGap + queueHeaderH + queueTitleGap + queueContentH + queueBotGap;

    const float actionBtnH = Panels::ButtonHeight(ImGuiEx::M3::Spec::SizeTips::SMALL);
    const float actionsH   = actionBtnH + padY;

    constexpr float kDividerH   = 1.0F;
    const float     fixedH      = statusH + kDividerH + kDividerH + queueH + actionsH;
    const float     minSpecimen = m3Styles.GetPixels(M3Spec::dp<72>());
    const float     specimenH   = std::max(size.y - fixedH, minSpecimen);
    // A pane shorter than the card's own fixed blocks must not squeeze the
    // card into an internal scroll — the bottom-anchored actions would float
    // over the scrolled specimen. Grow instead; the page scrolls.
    const float cardH = std::max(size.y, fixedH + minSpecimen);

    // ---- the card ----
    const auto styleGuard = ImGuiEx::StyleGuard()
                                .Color<ImGuiCol_ChildBg>(m3Styles.Colors()[M3Spec::ColorRole::surfaceContainerLowest])
                                .Color<ImGuiCol_Text>(m3Styles.Colors()[M3Spec::ColorRole::onSurface])
                                .Color<ImGuiCol_Border>(m3Styles.Colors()[M3Spec::ColorRole::outlineVariant])
                                .Style<ImGuiStyleVar_ChildRounding>(m3Styles.GetPixels(M3Spec::ShapeCorner::Medium))
                                .Style<ImGuiStyleVar_WindowPadding>(ImVec2(0.0F, 0.0F))
                                .Style<ImGuiStyleVar_ItemSpacing>(ImVec2(0.0F, 0.0F))
                                .Style<ImGuiStyleVar_ChildBorderSize>(1.0F);
    if (!ImGui::BeginChild("##FontResultCard", {size.x, cardH}, ImGuiEx::ChildFlags().Borders()))
    {
        return;
    }
    const ImVec2 cardMin  = ImGui::GetCursorScreenPos();
    const float  cardRight = cardMin.x + size.x;

    ResultCardLayout layout;
    layout.cardMin       = cardMin;
    layout.cardRight     = cardRight;
    layout.padX          = padX;
    layout.padY          = padY;
    layout.gap           = gap;
    layout.contentW      = contentW;
    layout.statusLineH   = statusLineH;
    layout.queueTitleH   = queueTitleH;
    layout.listLineH     = listLineH;
    layout.queueEmptyH   = queueEmptyH;
    layout.iconBtnH      = iconBtnH;
    layout.iconBtnW      = iconBtnW;
    layout.queueHeaderH  = queueHeaderH;
    layout.queueTitleGap = queueTitleGap;
    layout.queueTopGap   = queueTopGap;
    layout.rowH          = rowH;
    layout.rowCount      = rowCount;
    layout.queueContentH = queueContentH;
    layout.specimenH     = specimenH;
    layout.cardH         = cardH;
    layout.actionBtnH    = actionBtnH;

    DrawResultCardStatus(layout, m3Styles);
    DrawResultCardSpecimen(layout, m3Styles);
    DrawResultCardQueue(layout, fontBuilder, m3Styles);
    DrawResultCardActions(layout, fontBuilder, settings);

    ImGui::EndChild();
}

void FontPreviewPanel::DrawResultCardStatus(const ResultCardLayout &layout, ImGuiEx::M3::M3Styles &m3Styles)
{
    // Status strip: one quiet line explaining what the specimen shows.
    {
        const auto statusBar = [&]() -> StatusBar {
            switch (m_state)
            {
                case State::DEBOUNCING:
                    return {ICON_REFRESH_CW, std::string{Translate("Settings.FontBuilder.PreviewPanel.Debouncing")}};
                case State::PREVIEW_BUILDER_FONT:
                    return {ICON_EYE, std::string{Translate("Settings.FontBuilder.PreviewPanel.BuilderFont")}};
                case State::PREVIEWING:
                    return {ICON_FILE_CHECK, std::string{m_imFont.GetFontPathOr(0)}, M3Spec::ColorRole::onSurfaceVariant, true};
                case State::NOT_SUPPORTED_FONTS:
                    return {ICON_FILE_QUESTION_MARK, std::string{Translate("Settings.FontBuilder.PreviewPanel.NotSupportedFont")}, M3Spec::ColorRole::error};
                case State::EMPTY:
                case State::NOT_SELECTED_FONT:
                default:
                    return {ICON_CIRCLE_ALERT, std::string{Translate("Settings.FontBuilder.PreviewPanel.NotSelectedFont")}};
            }
        }();

        ImGui::SetCursorScreenPos({layout.cardMin.x + layout.padX, layout.cardMin.y + layout.padY});
        // Icon drawn directly on the line: the facade's Icon items submit a
        // 48dp hit box that would blow the strip's measured height and push
        // every block below it out of the card.
        const float statusIconSize = m3Styles.GetPixels(M3Spec::dp<16>());
        {
            const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::BodySmall>();
            const ImVec2 iconPos(layout.cardMin.x + layout.padX, layout.cardMin.y + layout.padY + (layout.statusLineH - statusIconSize) * 0.5F);
            ImGui::GetWindowDrawList()->AddText(
                m3Styles.IconFont(), statusIconSize, iconPos, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[statusBar.role]),
                statusBar.icon.data(), statusBar.icon.data() + statusBar.icon.size()
            );
            ImGui::Dummy({statusIconSize * 1.5F, layout.statusLineH});
            ImGui::SameLine(0.0F, layout.gap);
            // Long font paths elide to the strip; the tooltip discloses the full one.
            const float maxTextW = std::max(layout.contentW - statusIconSize * 1.5F - layout.gap, ImGui::GetTextLineHeight() * 4.0F);
            const auto  elided   = Panels::ElideText(statusBar.text, maxTextW);
            const std::string_view shownText = elided.empty() ? std::string_view{statusBar.text} : std::string_view{elided};
            ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::BodySmall>(shownText, statusBar.role);
            if (statusBar.tooltipPath)
            {
                ImGuiEx::M3::SetItemToolTip(statusBar.text);
            }
        }
        ImGui::Dummy({0.0F, layout.padY});
    }
    ImGui::SetCursorScreenPos({layout.cardMin.x, ImGui::GetCursorScreenPos().y});
    ImGuiEx::M3::Divider();
}

void FontPreviewPanel::DrawResultCardSpecimen(const ResultCardLayout &layout, ImGuiEx::M3::M3Styles &m3Styles)
{
    // Specimen: the preview text in the selected font, scrolling on its own.
    if (ImGui::BeginChild("##FontSpecimen", {0.0F, layout.specimenH}, ImGuiEx::ChildFlags()))
    {
        const float textPadX = m3Styles.GetPixels(M3Spec::TextParagraph::PaddingX);
        ImGui::Indent(textPadX);
        if (m_state == State::NOT_SUPPORTED_FONTS)
        {
            // A bare empty zone reads as broken; center the explanation instead.
            const auto   fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::BodyMedium>();
            const std::string_view message = Translate("Settings.FontBuilder.PreviewPanel.NotSupportedFont");
            const float  msgW = ImGui::CalcTextSize(ImGuiEx::TextStart(message), ImGuiEx::TextEnd(message)).x;
            const ImVec2 zoneMin = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({zoneMin.x + std::max((ImGui::GetContentRegionAvail().x - msgW) * 0.5F, 0.0F), zoneMin.y + std::max((layout.specimenH - ImGui::GetTextLineHeight()) * 0.5F, 0.0F)});
            ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::BodyMedium>(message, M3Spec::ColorRole::onSurfaceVariant);
        }
        else
        {
            DrawSpecimen();
        }
        ImGui::Unindent(textPadX);
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos({layout.cardMin.x, ImGui::GetCursorScreenPos().y});
    ImGuiEx::M3::Divider();
}

void FontPreviewPanel::DrawResultCardQueue(const ResultCardLayout &layout, FontBuilder &fontBuilder, ImGuiEx::M3::M3Styles &m3Styles)
{
    // Build queue: the ordered fallback chain, with the preview and add
    // actions anchored to its header — the two things you do from here.
    // The names were measured once in the caller (rowCount); the rows re-read
    // the same const accessor for their labels.
    const auto &builtNames = fontBuilder.GetBaseFont().GetFontNames();
    ImGui::Dummy({0.0F, layout.queueTopGap});
    const float headerY = ImGui::GetCursorScreenPos().y;
    {
        ImGui::SetCursorScreenPos({layout.cardMin.x + layout.padX, headerY + (layout.queueHeaderH - layout.queueTitleH) * 0.5F});
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::TitleSmall>(Translate("Settings.FontBuilder.BuildQueue"), M3Spec::ColorRole::onSurface);

        const float btnsW = layout.iconBtnW * 2.0F + layout.gap;
        ImGui::SetCursorScreenPos({layout.cardRight - layout.padX - btnsW, headerY + (layout.queueHeaderH - layout.iconBtnH) * 0.5F});
        ImGui::BeginDisabled(!fontBuilder.IsBuilding());
        if (ImGuiEx::M3::XSmallIconButton(ICON_EYE, ImGuiEx::M3::Spec::IconButtonColors::Standard))
        {
            PreviewFont(fontBuilder.GetBaseFont());
        }
        ImGui::EndDisabled();
        ImGuiEx::M3::SetItemToolTip(Translate("Settings.FontBuilder.PreviewBuild"));

        ImGui::SameLine(0.0F, layout.gap);
        ImGui::BeginDisabled(!m_imFont.IsCommittable());
        if (ImGuiEx::M3::XSmallIconButton(ICON_PLUS, ImGuiEx::M3::Spec::IconButtonColors::Tonal))
        {
            if (fontBuilder.AddFont(m_interactState.selectedIndex, m_imFont))
            {
                Cleanup();
            }
        }
        ImGui::EndDisabled();
        ImGuiEx::M3::SetItemToolTip(Translate("Settings.FontBuilder.Add"));
    }

    const float rowsY = headerY + layout.queueHeaderH + layout.queueTitleGap;
    if (layout.rowCount > 0)
    {
        ImGui::SetCursorScreenPos({layout.cardMin.x + layout.padX, rowsY});
        if (ImGui::BeginChild("##BuildQueueRows", {layout.contentW, layout.queueContentH}, ImGuiEx::ChildFlags()))
        {
            const auto fontScope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::List::textRole>();
            auto      *drawList  = ImGui::GetWindowDrawList();
            const auto &builtPaths = fontBuilder.GetBaseFont().GetFontPathList();
            const float numGap     = m3Styles.GetPixels(M3Spec::dp<8>());
            const float rowsMinX   = ImGui::GetCursorScreenPos().x;
            const float rowsTop    = ImGui::GetCursorScreenPos().y;
            for (int i = 0; i < layout.rowCount; ++i)
            {
                const ImVec2 rowMin(rowsMinX, rowsTop + static_cast<float>(i) * layout.rowH);
                ImGui::SetCursorScreenPos(rowMin);
                ImGui::Dummy({layout.contentW, layout.rowH});
                if (i < static_cast<int>(builtPaths.size()))
                {
                    ImGuiEx::M3::SetItemToolTip(builtPaths[static_cast<size_t>(i)]);
                }

                const std::string number = std::format("{}.", i + 1);
                const float       numW   = ImGui::CalcTextSize(number.c_str()).x;
                const float       textY  = rowMin.y + ImGuiEx::M3::CenteredTextOffsetY(layout.rowH);
                drawList->AddText(
                    {rowMin.x, textY}, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[M3Spec::ColorRole::onSurfaceVariant]), number.c_str()
                );
                const auto name = Panels::ElideText(builtNames[static_cast<size_t>(i)], layout.contentW - numW - numGap);
                drawList->AddText(
                    {rowMin.x + numW + numGap, textY},
                    ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[M3Spec::ColorRole::onSurface]),
                    name.empty() ? builtNames[static_cast<size_t>(i)].c_str() : name.c_str()
                );
            }
        }
        ImGui::EndChild();
    }
    else
    {
        ImGui::SetCursorScreenPos({layout.cardMin.x + layout.padX, rowsY});
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::BodySmall>(
            Translate("Settings.FontBuilder.BuildQueueEmpty"), M3Spec::ColorRole::onSurfaceVariant, layout.contentW
        );
    }
}

void FontPreviewPanel::DrawResultCardActions(const ResultCardLayout &layout, FontBuilder &fontBuilder, Settings &settings)
{
    // Actions anchored to the card's bottom edge.
    ImGui::SetCursorScreenPos({layout.cardMin.x + layout.padX, layout.cardMin.y + layout.cardH - layout.padY - layout.actionBtnH});
    ImGui::BeginDisabled(!fontBuilder.IsBuilding());
    {
        ImGuiEx::M3::ButtonConfiguration config;
        config.Filled().Icon(ICON_CHECK);
        config.Size(ImGuiEx::M3::Spec::SizeTips::SMALL);
        if (ImGuiEx::M3::Button(Translate("Settings.FontBuilder.SetAsDefault"), config))
        {
            (void)fontBuilder.ApplyFont(settings);
        }
    }
    ImGui::SameLine(0.0F, layout.gap);
    {
        ImGuiEx::M3::ButtonConfiguration config;
        config.Text().Icon(ICON_ROTATE_CCW);
        config.Size(ImGuiEx::M3::Spec::SizeTips::SMALL);
        if (ImGuiEx::M3::Button(Translate("Settings.FontBuilder.ResetBuilder"), config))
        {
            if (fontBuilder.GetBaseFont() == m_imFont)
            {
                Cleanup();
            }
            fontBuilder.Reset();
        }
    }
    ImGui::EndDisabled();
}

void FontPreviewPanel::UpdateDisplayFontInfos(const std::vector<FontInfo> &sourceList)
{
    m_displayFontInfos.clear();
    m_filterActive = m_textFilter.IsActive();
    for (const auto &fontInfo : sourceList)
    {
        if (m_textFilter.PassFilter(fontInfo.GetName().c_str()))
        {
            m_displayFontInfos.push_back(fontInfo);
        }
    }
}

auto FontPreviewPanel::PreviewFont(const std::string &fontName, const std::string &fontPath) -> bool
{
    auto *imFont = ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath.c_str());
    if (imFont == nullptr)
    {
        // AddFontFromFileTTF failed: the old code still marked PREVIEWING and
        // displayed the file path while actually rendering the default font.
        m_imFont.Cleanup();
        return false;
    }
    m_imFont = ImFontWrap(imFont, fontName, fontPath, true);
    return true;
}

void FontPreviewPanel::Cleanup()
{
    m_imFont.Cleanup();
    m_state = State::NOT_SELECTED_FONT;
}

} // namespace Ime::UI
