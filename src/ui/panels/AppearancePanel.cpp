//
// Created by jamie on 2026/1/27.
//

#include "ui/panels/AppearancePanel.h"

#include "cpp/scheme/scheme_tonal_spot.h"
#include "i18n/translator_manager.h"
#include "icons.h"
#include "imguiex/ImGuiEx.h"
#include "imguiex/Material3.h"
#include "imguiex/imguiex_enum_wrap.h"
#include "imguiex/imguiex_m3.h"
#include "imguiex/imguiex_m3_slider.h"
#include "imguiex/m3/facade/slider.h"
#include "imguiex/m3/spec/layout.h"
#include "imguiex/m3/spec/text_field.h"
#include "log.h"
#include "path_utils.h"
#include "toml/toml.hpp"
#include "ui/Settings.h"
#include "ui/panels/PanelWidgets.h"

#include <imgui.h>

#include <array>
#include <cstring>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace Ime
{

using ColorRole = M3Spec::ColorRole;

namespace
{
using Hct = material_color_utilities::Hct;

constexpr float kToneDefault = 50.F;
constexpr ImU32 COL_WHITE    = 0xFFFFFFFF;

void HandlerPickerCursor(ImDrawList *drawList, const char *strId, float &value, float maxValue, const ImVec2 &pickerPos, const ImVec2 &size)
{
    const float cursor_x = pickerPos.x + ((value / maxValue) * size.x);
    const float radius   = size.y * 0.5F;
    drawList->AddCircleFilled(ImVec2(cursor_x, pickerPos.y + radius), radius, COL_WHITE);

    const auto ratio = (ImGui::GetIO().MousePos.x - pickerPos.x) / size.x;

    (void)ImGui::InvisibleButton(strId, size);
    if (ImGui::IsItemActive())
    {
        value = maxValue * std::clamp(ratio, 0.0F, 1.0F);
    }
}

void DrawColorBar(ImDrawList *drawList, const ImVec2 &pos, const ImVec2 &size, const Hct &col1, const Hct &col2)
{
    drawList->AddRectFilledMultiColor(
        pos,
        ImVec2(pos.x + size.x, pos.y + size.y),
        ImGuiEx::M3::ArgbToImU32(col1.ToInt()),
        ImGuiEx::M3::ArgbToImU32(col2.ToInt()),
        ImGuiEx::M3::ArgbToImU32(col2.ToInt()),
        ImGuiEx::M3::ArgbToImU32(col1.ToInt())
    );
}

void DrawHuePicker(float &hue, float chroma, const ImVec2 &pickerSize)
{
    constexpr float kHueMax = 360.0F;

    const std::array col_hues = {
        Hct(0xFFFF0000), Hct(0xFFFFFF00), Hct(0xFF00FF00), Hct(0xFF00FFFF), Hct(0xFF0000FF), Hct(0xFFFF00FF), Hct(0xFFFF0000)
    };
    const auto kHueSegments = col_hues.size() - 1;

    auto *draw_list = ImGui::GetWindowDrawList();

    const ImVec2 picker_pos   = ImGui::GetCursorScreenPos();
    const float  segment_w    = pickerSize.x / static_cast<float>(kHueSegments);
    float        segment_minX = picker_pos.x;
    for (size_t i = 0; i < kHueSegments; ++i)
    {
        const auto col1 = Hct(col_hues[i].get_hue(), chroma, kToneDefault);
        const auto col2 = Hct(col_hues[i + 1].get_hue(), chroma, kToneDefault);

        DrawColorBar(draw_list, ImVec2(segment_minX, picker_pos.y), ImVec2(segment_w, pickerSize.y), col1, col2);

        segment_minX += segment_w;
    }

    HandlerPickerCursor(draw_list, "hue", hue, kHueMax, picker_pos, pickerSize);
}

void DrawChromaPicker(const float hue, float &chroma, const ImVec2 &pickerSize)
{
    constexpr float kChromaMin = 0.0F;
    constexpr float kChromaMax = 150.0F;

    auto *draw_list = ImGui::GetWindowDrawList();

    const ImVec2 picker_pos = ImGui::GetCursorScreenPos();

    const auto col1 = Hct(hue, kChromaMin, kToneDefault);
    const auto col2 = Hct(hue, kChromaMax, kToneDefault);
    DrawColorBar(draw_list, picker_pos, pickerSize, col1, col2);
    HandlerPickerCursor(draw_list, "chroma", chroma, kChromaMax, picker_pos, pickerSize);
}

void DrawTonePicker(const float hue, const float chroma, float &tone, const ImVec2 &pickerSize)
{
    constexpr float kToneMin = 0.0F;
    constexpr float kToneMax = 100.0F;
    constexpr int   segments = 3;
    constexpr float toneStep = kToneMax / static_cast<float>(segments);

    auto *draw_list = ImGui::GetWindowDrawList();

    const ImVec2 picker_pos = ImGui::GetCursorScreenPos();
    const float  segment_w  = pickerSize.x / static_cast<float>(segments);

    float segment_minX = picker_pos.x;
    float tone0        = kToneMin;
    for (int i = 0; i < segments; ++i)
    {
        const auto col1 = Hct(hue, chroma, tone0);
        const auto col2 = Hct(hue, chroma, tone0 + toneStep);

        DrawColorBar(draw_list, {segment_minX, picker_pos.y}, {segment_w, pickerSize.y}, col1, col2);

        tone0 += toneStep;
        segment_minX += segment_w;
    }

    HandlerPickerCursor(draw_list, "tone", tone, kToneMax, picker_pos, pickerSize);
}

void HexRgbInputText(AppearancePanel::HctCache &hctCache)
{
    const Hct   hct(hctCache.hue, hctCache.chroma, hctCache.tone);
    std::string buffer = std::format("#{:06X}", hct.ToInt() & 0xFFFFFFU);

    // resize (not reserve): the text field writes into the buffer up to the
    // size we hand it — editing past size() into reserved capacity was a
    // formal UB (harmless in practice, but not worth keeping).
    constexpr size_t BUFFER_SIZE = 64U;
    buffer.resize(BUFFER_SIZE);

    if (ImGuiEx::M3::OutlinedTextField("RGB", buffer.data(), buffer.size()))
    {
        // Shrink to the edited C-string first (strlen): the field null-
        // terminates its edit inside the buffer, and size() stays at 64.
        buffer.resize(std::strlen(buffer.data()));
        std::string_view view = std::string_view(buffer.data());
        for (const auto &c : view)
        {
            if (c == '#' || std::isspace(static_cast<unsigned char>(c)) != 0)
            {
                view.remove_prefix(1U);
                continue;
            }
            break;
        }

        std::array<int, 3U> color{};
        constexpr size_t    col_r_idx = 0U;
        constexpr size_t    col_g_idx = 1U;
        constexpr size_t    col_b_idx = 2U;

        const char *pHexColor = view.data();
        size_t      j         = 0U;
        while (j < color.size())
        {
            auto [p, err] = std::from_chars(pHexColor, pHexColor + 2, color[j++], 16);
            if (*p == '\0' || err != std::errc())
            {
                break;
            }
            pHexColor = p;
        }
        if (j == color.size())
        {
            auto new_Hct    = Hct(material_color_utilities::ArgbFromRgb(color[col_r_idx], color[col_g_idx], color[col_b_idx]));
            hctCache.hue    = static_cast<float>(new_Hct.get_hue());
            hctCache.chroma = static_cast<float>(new_Hct.get_chroma());
            hctCache.tone   = static_cast<float>(new_Hct.get_tone());
        }
    }
}

auto HctPickerPopup(const char *strId, AppearancePanel::HctCache &hctCache) -> bool
{
    bool applied = false;

    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    if (auto dialog = ImGuiEx::M3::DialogModal(strId); dialog)
    {
        HexRgbInputText(hctCache);

        const ImVec2 pickerSSize(ImGui::GetContentRegionAvail().x, m3Styles.GetPixels(M3Spec::SmallSlider::frameHeight));

        dialog.SupportingText(std::format("Hue: {:.3f}", hctCache.hue));
        DrawHuePicker(hctCache.hue, hctCache.chroma, pickerSSize);

        dialog.SupportingText(std::format("Chroma: {:.3f}", hctCache.chroma));
        DrawChromaPicker(hctCache.hue, hctCache.chroma, pickerSSize);

        dialog.SupportingText(std::format("Tone: {:.3f}", hctCache.tone));
        DrawTonePicker(hctCache.hue, hctCache.chroma, hctCache.tone, pickerSSize);

        if (dialog.ActionButton(Translate("Settings.Apply"), ICON_CHECK))
        {
            applied = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (dialog.ActionButton(Translate("Settings.Cancel"), ICON_X))
        {
            ImGui::CloseCurrentPopup();
        }
    }
    return applied;
}

//! Reads the top-level `LanguageName` key a translation file uses to declare
//! its own native display name (e.g. 中文 / 한국어) for the Languages dropdown.
//! An empty result means "not declared" — the caller falls back to the
//! internal name, which is also what a malformed third-party file degrades to.
auto ReadNativeLanguageName(const std::filesystem::path &dir, const std::string &language) -> std::string
{
    const auto file = dir / std::vformat(i18n::TRANSLATE_FILE_PATTERN, std::make_format_args(language));
    try
    {
        if (!std::filesystem::exists(file))
        {
            return {};
        }
        return toml::find_or(toml::parse(file.generic_string()), "LanguageName", std::string{});
    }
    catch (const std::exception &e)
    {
        logger::warn("Failed to read the display name of translation '{}': {}", file.generic_string(), e.what());
    }
    return {};
}

//! One Light/Dark segment of the theme mode selector; toggling rebuilds the
//! whole M3 palette. Same-frame style feedback is best-effort here — the
//! per-frame sync in ImeWnd::Draw makes the switch authoritative.
void DrawThemeModeSegment(const std::string_view label, const bool selectDark, Settings &settings)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    ImGuiEx::M3::ButtonConfiguration config;
    config.Tonal().Square();
    config.toggle   = true;
    config.Size(ImGuiEx::M3::Spec::SizeTips::XSMALL);
    config.selected = m3Styles.Colors().IsDark() == selectDark;
    if (ImGuiEx::M3::Button(label, config) && m3Styles.Colors().IsDark() != selectDark)
    {
        m3Styles.ToggleLightDarkScheme();
        ImGuiEx::M3::SetupDefaultImGuiStyles(ImGui::GetStyle());
        settings.appearance.schemeConfig.darkMode = m3Styles.Colors().IsDark();
    }
}
} // namespace

AppearancePanel::AppearancePanel()
{
    const auto translateDir = utils::GetInterfacePath() / SIMPLE_IME;
    i18n::ScanLanguages(translateDir, m_translateLanguages);
    for (const auto &language : m_translateLanguages)
    {
        if (auto name = ReadNativeLanguageName(translateDir, language); !name.empty())
        {
            m_nativeLanguageNames.emplace(language, std::move(name));
        }
    }
    // Translator lifecycle is managed by ToolWindow. See docs/adr/0001-toolwindow-translator-lifecycle.md
}

void AppearancePanel::Draw(Settings &settings)
{
    auto      &m3Styles   = ImGuiEx::M3::Context::GetM3Styles();
    const auto styleGuard = ImGuiEx::StyleGuard().Color<ImGuiCol_ChildBg>(m3Styles.Colors()[ColorRole::surface]);
    if (ImGui::BeginChild("##Appearance", {}, ImGuiEx::ChildFlags().AlwaysUseWindowPadding().AutoResizeY()))
    {
        UI::Panels::PageHeader(Translate("Settings.Sidebar.Display"), Translate("Settings.Page.Display.Support"));

        UI::Panels::SectionHeader(Translate("Settings.Appearance.CandidatePreview"));
        if (UI::Panels::BeginSettingsCard("##CandidatePreviewCard"))
        {
            DrawCandidatePreview(settings.appearance.verticalCandidateList);
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(Translate("Settings.Appearance.CandidateWindow"));
        if (UI::Panels::BeginSettingsCard("##CandidateWindowCard"))
        {
            (void)UI::Panels::SettingsToggleRow(
                "##VerticalCandidates", Translate("Settings.Appearance.VerticalCandidateList"), {}, settings.appearance.verticalCandidateList
            );
            (void)UI::Panels::SettingsToggleRow(
                "##LanguageBar", Translate("Settings.Appearance.AutoToggleLanguageBar"), {}, settings.appearance.autoToggleLanguageBar
            );
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(Translate("Settings.Behaviour.ImePos.Policy"));
        if (UI::Panels::BeginSettingsCard("##PolicyCard"))
        {
            DrawWindowPositionPolicy(settings);
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(Translate("Settings.Appearance.General"));
        if (UI::Panels::BeginSettingsCard("##GeneralCard"))
        {
            DrawZoomCombo(settings);
            DrawLanguagesCombo(settings.appearance);
        }
        UI::Panels::EndSettingsCard();

        UI::Panels::SectionHeader(Translate("Settings.Appearance.Theme"));
        if (UI::Panels::BeginSettingsCard("##ThemeCard"))
        {
            DrawThemeModeRow(settings);
            DrawThemeRow(settings);
        }
        UI::Panels::EndSettingsCard();
    }
    ImGui::EndChild();
}

void AppearancePanel::DrawCandidatePreview(const bool vertical) const
{
    auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float previewHeight = m3Styles.GetPixels(vertical ? M3Spec::dp<148>() : M3Spec::dp<84>());
    const auto  row = UI::Panels::BeginPlainSettingsRow({}, {}, 0.0F, previewHeight);
    if (!row)
    {
        return;
    }

    auto       *drawList = ImGui::GetWindowDrawList();
    const float inset    = m3Styles.GetPixels(M3Spec::dp<8>());
    const ImVec2 min(row.contentX, row.contentY);
    const ImVec2 max(row.trailingRight, row.contentY + previewHeight - inset);
    const float rounding = m3Styles.GetPixels(M3Spec::ShapeCorner::Medium);
    drawList->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::surfaceContainerLow]), rounding);
    drawList->AddRect(min, max, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::outlineVariant]), rounding);

    const auto drawText = [&](const std::string_view text, const ImVec2 position, const ColorRole role) {
        drawList->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(), position, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[role]), text.data(), text.data() + text.size()
        );
    };
    const float lineHeight = ImGui::GetTextLineHeight();
    const float innerX     = min.x + inset;
    const float innerY     = min.y + inset;

    if (vertical)
    {
        constexpr std::array<std::string_view, 6> candidates{"1 你好", "2 hello", "3 こんにちは", "4 안녕", "5 Hallo", "6 Привет"};
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            const float y = innerY + static_cast<float>(i) * (lineHeight + m3Styles.GetPixels(M3Spec::dp<2>()));
            drawText(candidates[i], {innerX, y}, i == 0 ? ColorRole::primary : ColorRole::onSurfaceVariant);
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
            if (x + width > maxWidth)
            {
                break;
            }
            if (i == 0)
            {
                drawList->AddRectFilled(
                    {x, y - padY}, {x + width, y + lineHeight + padY}, ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ColorRole::primary]),
                    m3Styles.GetPixels(M3Spec::ShapeCorner::Small)
                );
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

void AppearancePanel::DrawWindowPositionPolicy(Settings &settings) const
{
    using Policy = Settings::WindowPosUpdatePolicy;

    auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float gap      = m3Styles.GetPixels(M3Spec::List::segmentedGap);
    const float rowH     = UI::Panels::ButtonHeight();
    const float lineGap  = m3Styles.GetPixels(M3Spec::dp<8>());
    const float padX     = m3Styles.GetPixels(M3Spec::List::paddingX);

    const std::array<std::string_view, 3> labels{
        Translate("Settings.Behaviour.ImePos.UpdateByCursor"),
        Translate("Settings.Behaviour.ImePos.UpdateByCaret"),
        Translate("Settings.Behaviour.ImePos.UpdateByNone"),
    };
    const std::array<Policy, 3> values{Policy::BASED_ON_CURSOR, Policy::BASED_ON_CARET, Policy::NONE};
    const std::array<std::string_view, 3> tooltips{
        Translate("Settings.Behaviour.ImePos.UpdateByCursorTooltip"),
        Translate("Settings.Behaviour.ImePos.UpdateByCaretTooltip"),
        Translate("Settings.Behaviour.ImePos.UpdateByNoneTooltip"),
    };

    // Measure first, then wrap: three labeled segments overflow the card at
    // the minimum window width under long translations, so the run folds to
    // a second line instead of being clipped.
    std::vector<float> widths;
    widths.reserve(labels.size());
    for (const auto label : labels)
    {
        widths.push_back(UI::Panels::MeasureButton(label, {}, ImGuiEx::M3::Spec::SizeTips::XSMALL, ImGuiEx::M3::Spec::ButtonShape::Square));
    }
    const float contentW = ImGui::GetContentRegionAvail().x - padX * 2.0F;
    const auto  flow     = UI::Panels::FlowLayoutPositions({}, contentW, gap, lineGap, rowH, widths);

    const auto row = UI::Panels::BeginPlainSettingsRow({}, {}, 0.0F, flow.height);
    if (!row)
    {
        return;
    }
    for (size_t i = 0; i < labels.size(); ++i)
    {
        ImGui::SetCursorScreenPos({row.contentX + flow.positions[i].x, row.contentY + flow.positions[i].y});
        ImGuiEx::M3::ButtonConfiguration config;
        config.Tonal().Square();
        config.toggle   = true;
        config.Size(ImGuiEx::M3::Spec::SizeTips::XSMALL);
        config.selected = settings.input.posUpdatePolicy == values[i];
        if (ImGuiEx::M3::Button(labels[i], config))
        {
            settings.input.posUpdatePolicy = values[i];
        }
        ImGuiEx::M3::SetItemToolTip(tooltips[i]);
    }

    UI::Panels::EndSettingsRow(row);
}

void AppearancePanel::DrawZoomCombo(Settings &settings)
{
    namespace M3 = ImGuiEx::M3;
    if (!m_zoomPercentSynced)
    {
        // Seed the display from the actual configured zoom once: the panel is
        // recreated per window-open, and the combo used to show "100%" forever
        // even when the config held 1.25/1.5 (or -1 = follow the monitor).
        m_zoomPercentSynced = true;
        m_currentZoomPercent =
            settings.appearance.zoom > 0.0F
                ? static_cast<uint32_t>(std::round(settings.appearance.zoom * 100.F))
                : ZOOM_DEFAULT_PERCENT;
    }

    const std::string preview = settings.appearance.zoom < 0.0F ? std::string(Translate("Settings.Appearance.ZoomFollowMonitor")) : std::format("{}%", m_currentZoomPercent);
    // The combo reserves its exact width so the row title wraps short of the
    // chip instead of colliding under long "follow monitor" translations.
    UI::Panels::SettingsComboRow(
        "##ZoomCombo",
        Translate("Settings.Appearance.Zoom"),
        {},
        preview,
        [&] {
            const bool followsMonitor = settings.appearance.zoom < 0.0F;
            if (UI::Panels::ComboOption(Translate("Settings.Appearance.ZoomFollowMonitor"), followsMonitor))
            {
                settings.appearance.zoom                 = -1.0F;
                settings.runtimeData.requestMonitorScale = true;
            }

            for (uint32_t zoom = ZOOM_MIN_PERCENT; zoom <= ZOOM_MAX_PERCENT; zoom += ZOOM_STEP_PERCENT)
            {
                const bool selected = settings.appearance.zoom > 0.0F &&
                                      zoom == static_cast<uint32_t>(std::round(settings.appearance.zoom * 100.F));
                if (UI::Panels::ComboOption(std::format("{}%", zoom), selected))
                {
                    const float uiScale = static_cast<float>(zoom) / 100.F;
                    ImGuiEx::M3::Context::GetM3Styles().UpdateScaling(uiScale);
                    // The global style caches scaled values (roundings, scrollbar size):
                    // rebuild it so they follow the new scale.
                    ImGuiEx::M3::SetupDefaultImGuiStyles(ImGui::GetStyle());
                    settings.appearance.zoom = uiScale;
                    m_currentZoomPercent     = zoom;
                }
            }
        });
}

void AppearancePanel::DrawThemeRow(Settings &settings)
{
    auto       &m3Styles        = ImGuiEx::M3::Context::GetM3Styles();
    const auto &schemeConfig    = m3Styles.Colors().GetSchemeConfig();
    constexpr auto colorButtonFlags = ImGuiEx::ColorEditFlags().NoAlpha().AlphaOpaque().NoPicker().NoTooltip();

    // Seed EVERY field the dialog can edit, not just hue/chroma/tone:
    // m_configuredContrastLevel used to keep its stale 0.0, so merely
    // changing the color and pressing Apply silently reset the user's
    // configured contrast. The scheme is built immediately so the dialog
    // shows the palette preview on its first frame.
    const auto seedBuilderState = [&] {
        const Hct hct(schemeConfig.sourceColor);
        m_configuredHct.hue    = static_cast<float>(hct.get_hue());
        m_configuredHct.chroma = static_cast<float>(hct.get_chroma());
        m_configuredHct.tone   = static_cast<float>(hct.get_tone());

        m_configuredVariant       = schemeConfig.variant;
        m_configuredColorEdited   = false;
        m_configuredDarkMode      = schemeConfig.darkMode;
        m_configuredContrastLevel = schemeConfig.contrastLevel;
        m_configuredScheme =
            std::make_unique<Scheme>(Hct(m_configuredHct.hue, m_configuredHct.chroma, m_configuredHct.tone), m_configuredDarkMode, m_configuredContrastLevel);
    };

    const std::string_view customizeLabel = Translate("Settings.Appearance.Customize");
    const std::string_view resetLabel     = Translate("Settings.Appearance.ResetTheme");
    const float            swatchSize     = m3Styles.GetPixels(M3Spec::dp<40>());
    const float            buttonHeight   = UI::Panels::ButtonHeight();
    const float            buttonGap      = m3Styles.GetPixels(M3Spec::dp<8>());
    const float            customizeWidth = UI::Panels::MeasureButton(customizeLabel, ICON_PALETTE);
    // Reset is a quiet action, same weight as the shortcut row's reset link;
    // Customize stays the row's only button.
    const float resetWidth  = UI::Panels::TextLinkWidth(resetLabel);
    const float trailingReserve = customizeWidth + buttonGap + resetWidth;

    const auto row = UI::Panels::BeginPlainSettingsRow({}, {}, trailingReserve, swatchSize);
    if (row)
    {
        ImGui::SetCursorScreenPos({row.contentX, row.centerY - swatchSize * 0.5F});
        if (ImGui::ColorButton("##SourceColor", ImGuiEx::M3::ArgbToImVec4(schemeConfig.sourceColor), colorButtonFlags, {swatchSize, swatchSize}))
        {
            seedBuilderState();
            ImGui::OpenPopup("ThemeBuilder");
        }

        UI::Panels::RowTitle(row, Translate("Settings.Appearance.Theme"), swatchSize + m3Styles.GetPixels(M3Spec::dp<16>()));

        ImGui::SetCursorScreenPos({row.trailingRight - trailingReserve, row.centerY - buttonHeight * 0.5F});
        if (ImGuiEx::M3::XSmallButton(customizeLabel, ICON_PALETTE))
        {
            seedBuilderState();
            ImGui::OpenPopup("ThemeBuilder");
        }

        if (UI::Panels::RowTrailingTextLink(row, "##ResetTheme", resetLabel))
        {
            const ImGuiEx::M3::SchemeConfig defaultScheme = ImGuiEx::M3::GetDefaultSchemeConfig(m3Styles.Colors().IsDark());
            m3Styles.RebuildColors(defaultScheme);
            ImGuiEx::M3::SetupDefaultImGuiStyles(ImGui::GetStyle());
            settings.appearance.schemeConfig = defaultScheme;
            m_configuredScheme.reset();
        }
        ImGuiEx::M3::SetItemToolTip(Translate("Settings.Appearance.ResetThemeToolTip"));

        UI::Panels::EndSettingsRow(row);
    }

    DrawThemeBuilderDialog(settings);
}

void AppearancePanel::DrawThemeBuilderDialog(Settings &settings)
{
    constexpr auto HCT_PICKER_POPUP = "##HctPickerPopup";

    auto      &m3Styles           = ImGuiEx::M3::Context::GetM3Styles();
    constexpr auto colorButtonFlags = ImGuiEx::ColorEditFlags().NoAlpha().AlphaOpaque().NoPicker().NoTooltip();

    if (auto dialog = ImGuiEx::M3::DialogModal(Translate("Settings.Appearance.ThemeBuilder")); dialog)
    {
        const auto hctPickerPopupId = ImGui::GetID(HCT_PICKER_POPUP);

        bool edited = false;
        if (HctPickerPopup(HCT_PICKER_POPUP, m_configuredHct))
        {
            m_configuredColorEdited = true;
            edited                  = true;
        }
        // No dark-mode control here: the Light/Dark segmented row in the theme
        // card is the single entry point for the mode (it applies instantly),
        // and the dialog always previews in the mode that is currently active.
        edited = ImGuiEx::M3::Slider::Draw(
                     Translate("Settings.Appearance.ContrastLevel"),
                     ImGuiEx::M3::Slider::Params{
                         .value = m_configuredContrastLevel, .minValue = ImGuiEx::M3::CONTRAST_MIN, .maxValue = ImGuiEx::M3::CONTRAST_MAX
                     }
                 ) ||
                 edited;
        if (edited)
        {
            m_configuredScheme = std::make_unique<Scheme>(
                Hct(m_configuredHct.hue, m_configuredHct.chroma, m_configuredHct.tone), m_configuredDarkMode, m_configuredContrastLevel
            );
        }

        if (m_configuredScheme)
        {
            // The source color button is the dialog's only palette row. The other
            // M3 roles (secondary/tertiary/neutral/…) used to render here as
            // preview-only swatches, but they derive from the source color and are
            // not editable, so users read them as broken buttons — removed.
            const auto paletteSize = ImGuiEx::M3::ListLeadingImageSize();

            ImGuiEx::M3::ListItem([&] -> void {
                if (ImGui::ColorButton(
                        "Primary",
                        ImGuiEx::M3::ArgbToImVec4(m_configuredScheme->primary_palette.get_key_color().ToInt()),
                        colorButtonFlags,
                        paletteSize
                    ))
                {
                    ImGui::OpenPopup(hctPickerPopupId);
                }
                ImGui::SameLine();
                ImGuiEx::M3::AlignedLabel("Primary");
            });
        }

        if (dialog.ActionButton(Translate("Settings.Apply"), ICON_CHECK))
        {
            // The curated default palette ignores a custom source color, so a
            // committed color edit means leaving it for the dynamic scheme;
            // dark-mode/contrast-only changes keep the current variant.
            const bool keepDefault = m_configuredVariant == ImGuiEx::M3::ThemeVariant::Default && !m_configuredColorEdited;
            const ImGuiEx::M3::SchemeConfig schemeConfig =
                keepDefault
                    ? ImGuiEx::M3::GetDefaultSchemeConfig(m_configuredDarkMode)
                    : ImGuiEx::M3::SchemeConfig{
                          .contrastLevel = m_configuredContrastLevel,
                          .sourceColor   = Hct(m_configuredHct.hue, m_configuredHct.chroma, m_configuredHct.tone).ToInt(),
                          .darkMode      = m_configuredDarkMode
                      };
            m3Styles.RebuildColors(schemeConfig);
            ImGuiEx::M3::SetupDefaultImGuiStyles(ImGui::GetStyle());
            settings.appearance.schemeConfig = schemeConfig;
            m_configuredScheme.reset();
        }

        ImGui::SameLine();
        (void)dialog.ActionButton(Translate("Settings.Cancel"), ICON_X);
    }
}

//! Light/dark scheme choice as a settings row: a two-segment mode selector in
//! the theme card, replacing the former dark-mode toggle. Selecting a mode
//! rebuilds the whole M3 palette; the scheme config is synced back into
//! settings by DrawMenuAppearance while this panel is open. The style refresh
//! here is best-effort same-frame feedback — the per-frame style sync in
//! ImeWnd::Draw is what makes the switch authoritative.
void AppearancePanel::DrawThemeModeRow(Settings &settings)
{
    using ImGuiEx::M3::Spec::ButtonShape;
    using ImGuiEx::M3::Spec::SizeTips;

    auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const float gap      = m3Styles.GetPixels(M3Spec::List::segmentedGap);
    const float rowH     = UI::Panels::ButtonHeight();
    const float lineGap  = m3Styles.GetPixels(M3Spec::dp<8>());
    const float padX     = m3Styles.GetPixels(M3Spec::List::paddingX);

    const std::array<std::string_view, 2> labels{
        Translate("Settings.Appearance.Light"),
        Translate("Settings.Appearance.Dark"),
    };
    std::vector<float> widths;
    widths.reserve(labels.size());
    for (const auto label : labels)
    {
        widths.push_back(UI::Panels::MeasureButton(label, {}, SizeTips::XSMALL, ButtonShape::Square));
    }

    // The title and the segment run share the row; when long translations
    // push the run past the card, the run moves below the title and flows.
    const std::string_view title = Translate("Settings.Appearance.ThemeMode");
    const float titleW = UI::Panels::MeasureListText(title);
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
        const auto  row      = UI::Panels::BeginPlainSettingsRow(
            Translate("Settings.Appearance.ThemeMode"), {}, 0.0F, titleLineH + titleGap + flow.height);
        if (!row)
        {
            return;
        }
        UI::Panels::RowTitle(row);
        for (size_t i = 0; i < labels.size(); ++i)
        {
            ImGui::SetCursorScreenPos({row.contentX + flow.positions[i].x, row.contentY + titleLineH + titleGap + flow.positions[i].y});
            DrawThemeModeSegment(labels[i], i == 1, settings);
        }
        UI::Panels::EndSettingsRow(row);
    }
    else
    {
        const auto row = UI::Panels::BeginPlainSettingsRow(Translate("Settings.Appearance.ThemeMode"), {}, runW, rowH);
        if (!row)
        {
            return;
        }
        UI::Panels::RowTitle(row);
        ImGui::SetCursorScreenPos({row.trailingRight - runW, row.centerY - rowH * 0.5F});
        DrawThemeModeSegment(labels[0], false, settings);
        ImGui::SameLine(0.0F, gap);
        DrawThemeModeSegment(labels[1], true, settings);
        ImGui::NewLine();
        UI::Panels::EndSettingsRow(row);
    }
}

void AppearancePanel::DrawLanguagesCombo(Settings::Appearance &appearance) const
{
    //! Show each language under its native name ("chinese" -> 中文) as declared
    //! by its file's LanguageName key; undeclared ones keep the internal name.
    const auto displayName = [&](const std::string &language) -> std::string_view {
        if (const auto it = m_nativeLanguageNames.find(language); it != m_nativeLanguageNames.end())
        {
            return it->second;
        }
        return language;
    };

    const std::string preview{displayName(appearance.language)};
    bool   clicked = false;
    // The combo reserves its exact width so the title never runs under the chip.
    UI::Panels::SettingsComboRow(
        "##LanguagesCombo",
        Translate("Settings.Appearance.Languages"),
        {},
        preview,
        [&] {
            int32_t idx = 0;
            for (const auto &language : m_translateLanguages)
            {
                ImGui::PushID(idx);
                const bool isSelected = appearance.language == language;
                const std::string name{displayName(language)};
                if (UI::Panels::ComboOption(name, isSelected))
                {
                    appearance.language = language;
                    clicked             = true;
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::PopID();
                idx++;
            }
        });

    if (clicked)
    {
        i18n::UpdateTranslator(appearance.language, "english", utils::GetPluginInterfaceDir());
    }
}
} // namespace Ime
