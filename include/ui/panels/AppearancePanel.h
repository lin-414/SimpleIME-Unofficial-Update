//
// Created by jamie on 2026/1/27.
//

#pragma once

#include "cpp/scheme/scheme_tonal_spot.h"
#include "ui/Settings.h"

#include <unordered_map>

namespace Ime
{
class AppearancePanel
{
    using Scheme                                   = material_color_utilities::SchemeTonalSpot;
    static constexpr uint32_t ZOOM_STEP_PERCENT    = 25U;
    static constexpr uint32_t ZOOM_MIN_PERCENT     = 50U;
    static constexpr uint32_t ZOOM_MAX_PERCENT     = 200U;
    static constexpr uint32_t ZOOM_DEFAULT_PERCENT = 100U;

public:
    //! Cache each component of HCT separately, because each component may abruptly
    //! change at the boundary, such as: HUE 360 -> 0.
    struct HctCache
    {
        float hue;
        float chroma;
        float tone;
    };

private:
    std::vector<std::string> m_translateLanguages;
    /// Internal language name -> native display name ("chinese" -> 中文),
    /// read from each file's top-level LanguageName key at construction.
    /// Languages whose file omits the key fall back to the internal name.
    std::unordered_map<std::string, std::string> m_nativeLanguageNames;
    std::unique_ptr<Scheme>  m_configuredScheme{nullptr};
    HctCache                 m_configuredHct{};
    /// Variant the theme had when the builder dialog opened. Apply keeps it
    /// (the default palette stays default) unless the color was edited this
    /// session — the curated palette ignores a custom source color, so editing
    /// the color means switching to the dynamic TonalSpot scheme.
    ImGuiEx::M3::ThemeVariant m_configuredVariant = ImGuiEx::M3::ThemeVariant::Default;
    bool                     m_configuredColorEdited = false; ///< the HCT picker committed a new color since the dialog opened
    uint32_t                 m_currentZoomPercent      = ZOOM_DEFAULT_PERCENT;
    bool                     m_zoomPercentSynced       = false; ///< seed the combo from settings on first Draw
    double                   m_configuredContrastLevel = 0.0;
    bool                     m_configuredDarkMode      = false;

public:
    explicit AppearancePanel();

    void Draw(Settings &settings);

private:
    void DrawZoomCombo(Settings &settings);
    void DrawThemeModeRow(Settings &settings);
    void DrawThemeRow(Settings &settings);
    void DrawCandidatePreview(bool vertical) const;
    void DrawWindowPositionPolicy(Settings &settings) const;
    void DrawThemeBuilderDialog(Settings &settings);
    void DrawLanguagesCombo(Settings::Appearance &appearance) const;
};

} // namespace Ime
