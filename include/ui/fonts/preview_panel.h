//
// Created by jamie on 2026/1/16.
//

#pragma once

#include "ui/DebounceTimer.h"
#include "ui/fonts/FontBuilder.h"
#include "ui/fonts/ImFontWrap.h"

#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <string>
#include <vector>

namespace ImGuiEx::M3
{
class M3Styles;
}

struct ImGuiTextFilter;

namespace Ime::UI
{
class FontPreviewPanel
{
    ImGuiTextFilter       m_textFilter;
    ImFontWrap            m_imFont{};
    std::vector<FontInfo> m_displayFontInfos;
    /// True while the search filter is in use: an empty m_displayFontInfos
    /// then means "no match", not "no filter applied".
    bool                  m_filterActive = false;
    DebounceTimer         m_previewDebounceTimer{std::chrono::milliseconds{300}};
    DebounceTimer         m_searchDebounceTimer{std::chrono::milliseconds{200}};

    enum class State : int8_t
    {
        EMPTY = 0,
        PREVIEWING,
        DEBOUNCING,
        NOT_SUPPORTED_FONTS,
        NOT_SELECTED_FONT,
        PREVIEW_BUILDER_FONT,
    };

    State m_state = State::EMPTY;

public:
    FontPreviewPanel() = default;

    struct InteractState
    {
        bool didInteract   = false;
        int  selectedIndex = -1;
    };

    //! Left picker pane: search bar pinned on top, the installed-font list
    //! filling every remaining pixel of `size` in its own scroll view.
    void Draw(FontBuilder &fontBuilder, const ImVec2 &size);

    //! Right result card: status strip, scrolling specimen in the selected
    //! font, the ordered build queue and its actions — sized to fill `size`.
    void DrawResultCard(FontBuilder &fontBuilder, Settings &settings, ImGuiEx::M3::M3Styles &m3Styles, const ImVec2 &size);

private:
    void DrawSearchBar(const std::vector<FontInfo> &fontInfos);
    void DrawFontsList(const std::vector<FontInfo> &fontInfos);
    void DrawSpecimen();
    void UpdateDisplayFontInfos(const std::vector<FontInfo> &sourceList);

public:
    [[nodiscard]] auto IsPreviewing() const -> bool { return m_state == State::PREVIEWING; }

private:
    auto PreviewFont(const std::string &fontName, const std::string &fontPath) -> bool;

public:
    void PreviewFont(const ImFontWrap &imFont)
    {
        m_imFont = imFont;
        m_state  = State::PREVIEW_BUILDER_FONT;
    }

    void Cleanup();

    [[nodiscard]] auto GetInteractState() const -> const InteractState & { return m_interactState; }

    [[nodiscard]] auto GetImFont() const -> const ImFontWrap & { return m_imFont; }

    [[nodiscard]] auto GetImFont() -> ImFontWrap & { return m_imFont; }

private:
    InteractState m_interactState;
};
} // namespace Ime::UI
