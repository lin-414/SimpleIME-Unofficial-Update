//
// Created by jamie on 2026/3/10.
//

#pragma once

#include "Settings.h"
#include "configs/ConfigSerializer.h"
#include "fonts/FontBuilder.h"
#include "fonts/FontBuilderPanel.h"
#include "hooks/SupportState.h"
#include "panels/AppearancePanel.h"

#include <optional>

namespace Ime::UI
{
/**
 * TODO: may show a state info in AppBar or other place? [low priority]
 * show current input method?
 * @note Initialize FontManager will be time-consuming operation, it will load all installed system font by DWrite API during constructing.
 */
class ToolWindow
{
public:
    enum class Menu : int8_t
    {
        InputStatus,
        Display,
        FontBuilder,
        Advanced,
    };

private:
    FontBuilder      m_fontBuilder;
    FontBuilderPanel m_fontBuilderView{};
    AppearancePanel  m_panelAppearance{};
    Menu             m_currentMenu = Menu::InputStatus;
    /// Render thread only: the Behaviour-panel shortcut rebind is armed and waiting
    /// for a key chord (Settings::RuntimeData::swallowShortcutToggle mirrors it).
    bool m_capturingShortcut = false;
    /// Render thread only: a capture just completed and the captured chord is
    /// still physically held — the swallow latch stays armed until every chord
    /// key is released, so a re-press of the main key before letting go of the
    /// modifiers cannot toggle the overlay out from under the settings window.
    bool m_capturedChordAwaitRelease = false;
    /// Render thread only: the on-disk configuration validated lazily on the
    /// first draw of the Advanced page (one small TOML parse per window open).
    std::optional<ConfigSerializer::ConfigStatus> m_configStatus;
    /// Render thread only: ImGui::GetTime() of the last log-path copy, driving
    /// the brief "copied" acknowledgment on the Advanced page.
    float m_logCopiedAt = -1.0F;
    /// Render thread only: same pattern as m_logCopiedAt, for the diagnostics
    /// copy row on the Advanced page.
    float m_diagCopiedAt = -1.0F;

public:
    ToolWindow();
    ~ToolWindow();

    ToolWindow(const ToolWindow &)            = delete;
    ToolWindow &operator=(const ToolWindow &) = delete;
    ToolWindow(ToolWindow &&)                 = delete;
    ToolWindow &operator=(ToolWindow &&)      = delete;

    void Draw(Settings &settings);

    /// Abort an armed shortcut capture. Called when the window is hidden without
    /// being destroyed, so a re-opened window does not resume capturing.
    void CancelShortcutCapture()
    {
        m_capturingShortcut         = false;
        m_capturedChordAwaitRelease = false;
    }

private:
    void        DrawSettingsHeader(Settings &settings);
    void        DrawSidebar();
    void        DrawMenuAppearance(Settings &settings);
    void        DrawMenuFontBuilder(Settings &settings);
    void        DrawMenuInputStatus(Settings &settings);
    void        DrawMenuAdvanced(Settings &settings);
    void        DrawStatusCard(const Settings &settings) const;
    void        DrawShortcutSection(Settings &settings);
    void        DrawLogPathRow();
    void        DrawConfigStatusRow();
    void        DrawDiagnosticsRow(const Settings &settings);
    void        DrawLogLevelRow(Settings &settings);
    void        DrawErrorDurationRow(Settings &settings);
    void        DrawEnvironmentRows(const Settings &settings);
    std::string BuildDiagnosticsText(const Settings &settings) const;
};
} // namespace Ime::UI
