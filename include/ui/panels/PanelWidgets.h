//
// Created by jamie on 2026/10/2.
//

#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include "imguiex/imguiex_m3.h"
#include "imguiex/m3/spec/buttons.h"
#include "imguiex/m3/spec/checkbox.h"
#include "imguiex/m3/spec/color_roles.h"
#include "imguiex/m3/spec/others.h"
#include "imguiex/m3/spec/shapes.h"
#include "imguiex/m3/spec/text_field.h"
#include "imguiex/m3/spec/typography.h"
#include "icons.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ime::UI::Panels
{

//! Section label above a settings card: TitleSmall in the primary text tone —
//! the cards under it do the grouping, the header only names them.
inline void SectionHeader(const std::string_view title)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    ImGui::Dummy({0.F, m3Styles.GetPixels(M3Spec::dp<12>())});
    ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::TitleSmall>(title, ImGuiEx::M3::Spec::ColorRole::onSurface);
    ImGui::Dummy({0.F, m3Styles.GetPixels(M3Spec::dp<4>())});
}

namespace Detail
{
//! Defined further down; declared here so StatusDot (below) can pick up the
//! disabled-content alpha. The default argument lives on this declaration
//! only — repeating it on the definition is a redefinition error.
auto ContentColor(const ImGuiEx::M3::Spec::ColorRole role, const float alpha = 1.0F) -> ImVec4;
} // namespace Detail

//! Small state dot before a status label: the theme accent when on, the
//! disabled tone when off. Inside a BeginDisabled scope (e.g. the dependent
//! summary toggles) the fill follows the M3 38% disabled content alpha.
inline void StatusDot(const bool on)
{
    auto       *drawList = ImGui::GetWindowDrawList();
    const float radius   = ImGui::GetTextLineHeight() * 0.30F;
    const auto  cursor   = ImGui::GetCursorScreenPos();
    const float centerY  = cursor.y + ImGui::GetTextLineHeight() * 0.5F;
    const auto  color    = ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(
        on ? ImGuiEx::M3::Spec::ColorRole::primary : ImGuiEx::M3::Spec::ColorRole::outline
    ));
    drawList->AddCircleFilled(ImVec2(cursor.x + radius, centerY), radius, color);
    ImGui::Dummy(ImVec2(radius * 2.0F, ImGui::GetTextLineHeight()));
}

//! Width of one physical keycap of a shortcut chord, so rows can measure the
//! whole keycap run before their extent commits.
inline auto KeycapWidth(const std::string_view label) -> float
{
    const float height = ImGui::GetFrameHeight();
    const float padX   = height * 0.45F;
    return ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + padX * 2.0F;
}

//! One physical keycap of a shortcut chord: recessed rounded rect with a
//! hairline edge, sized to the label.
inline void Keycap(const std::string_view label)
{
    auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    auto       *drawList = ImGui::GetWindowDrawList();
    const auto  pos      = ImGui::GetCursorScreenPos();
    const float height   = ImGui::GetFrameHeight();
    const auto  textSize = ImGui::CalcTextSize(label.data(), label.data() + label.size());
    const float padX     = height * 0.45F;
    const ImVec2 size(textSize.x + padX * 2.0F, height);
    const float rounding = ImGui::GetStyle().FrameRounding;

    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    drawList->AddRectFilled(
        pos,
        max,
        ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ImGuiEx::M3::Spec::ColorRole::surfaceContainerLowest]),
        rounding
    );
    drawList->AddRect(
        pos,
        max,
        ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ImGuiEx::M3::Spec::ColorRole::outlineVariant]),
        rounding
    );
    drawList->AddText(
        ImVec2(pos.x + padX, pos.y + ImGuiEx::M3::CenteredTextOffsetY(height)),
        ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ImGuiEx::M3::Spec::ColorRole::onSurface]),
        label.data(),
        label.data() + label.size()
    );
    ImGui::Dummy(size);
}

//! UTF-8 lead-byte length of the sequence starting at `c` (ASCII and stray
//! continuation bytes count as 1 — callers walk from code-point boundaries).
inline auto Utf8SequenceLength(const unsigned char c) -> size_t
{
    if (c < 0x80U)
    {
        return 1;
    }
    if ((c & 0xE0U) == 0xC0U)
    {
        return 2;
    }
    if ((c & 0xF0U) == 0xE0U)
    {
        return 3;
    }
    if ((c & 0xF8U) == 0xF0U)
    {
        return 4;
    }
    return 1;
}

//! The label truncated at a code-point boundary with a trailing ellipsis so it
//! fits `maxWidth` px; empty when the label already fits. Compact UI surfaces
//! with a fixed width (sidebar rail, combo chip) elide instead of clipping
//! mid-glyph under long translations — pair with a tooltip that discloses the
//! full text. Must run inside a UseTextRole scope (measurement follows the
//! active role font).
inline auto ElideText(const std::string_view text, const float maxWidth) -> std::string
{
    constexpr std::string_view kEllipsis = "…";
    if (maxWidth <= 0.0F || ImGui::CalcTextSize(ImGuiEx::TextStart(text), ImGuiEx::TextEnd(text)).x <= maxWidth)
    {
        return {};
    }
    const float ellipsisWidth = ImGui::CalcTextSize(kEllipsis.data(), kEllipsis.data() + kEllipsis.size()).x;
    float       usedWidth     = 0.0F;
    size_t      cut           = 0;
    for (size_t i = 0; i < text.size();)
    {
        const size_t charLen = Utf8SequenceLength(static_cast<unsigned char>(text[i]));
        const size_t charEnd = std::min(i + charLen, text.size());
        const float  charWidth =
            ImGui::CalcTextSize(ImGuiEx::TextStart(text) + i, ImGuiEx::TextStart(text) + charEnd).x;
        if (usedWidth + charWidth + ellipsisWidth > maxWidth)
        {
            break;
        }
        usedWidth += charWidth;
        i          = charEnd;
        cut        = charEnd;
    }
    return std::string(text.substr(0, cut)) + std::string(kEllipsis);
}

//! Width of an M3 button as the facade will render it, so trailing button
//! groups can be right-anchored without a layout pass.
inline auto MeasureButton(
    const std::string_view               label,
    const std::string_view               icon,
    const ImGuiEx::M3::Spec::SizeTips    size  = ImGuiEx::M3::Spec::SizeTips::XSMALL,
    const ImGuiEx::M3::Spec::ButtonShape shape = ImGuiEx::M3::Spec::ButtonShape::Round
) -> float
{
    using namespace ImGuiEx::M3;
    auto       &m3Styles  = Context::GetM3Styles();
    const auto  sizing    = Spec::GetButtonSizing(size, shape);
    const auto  fontScope = m3Styles.UseTextRole(sizing.labelText);
    float       width     = ImGui::CalcTextSize(ImGuiEx::TextStart(label), ImGuiEx::TextEnd(label)).x;
    width += m3Styles.GetPixels(sizing.leadingSpace) * 2.0F;
    if (!icon.empty())
    {
        width += m3Styles.GetPixels(sizing.iconSize) + m3Styles.GetPixels(sizing.iconLabelSpace);
    }
    return width;
}

//! Height of an M3 button of the given sizing.
inline auto ButtonHeight(const ImGuiEx::M3::Spec::SizeTips size = ImGuiEx::M3::Spec::SizeTips::XSMALL) -> float
{
    using namespace ImGuiEx::M3;
    return Context::GetM3Styles().GetPixels(Spec::GetButtonSizing(size, Spec::ButtonShape::Round).containerHeight);
}

//!==========================================================================
//! Flow layout: places a measured run of items left to right, wrapping to
//! the next line when the next item would exceed the available width. Rows
//! that position labeled controls horizontally (segmented choices, status
//! chips, the summary toggles) use it so long translations fold to another
//! line instead of overflowing the card. Pure geometry: nothing is drawn or
//! submitted, so it can run before the row's height commits.
//!==========================================================================
struct FlowLayout
{
    std::vector<ImVec2> positions{}; ///< top-left of each item
    float               height = 0;  ///< total height of all lines
};

inline auto FlowLayoutPositions(
    const ImVec2             &origin,
    const float               availWidth,
    const float               gapX,
    const float               lineGap,
    const float               lineHeight,
    const std::vector<float> &itemWidths
) -> FlowLayout
{
    FlowLayout  layout;
    const float lineMaxX    = origin.x + availWidth;
    float       x           = origin.x;
    float       y           = origin.y;
    bool        firstOnLine = true;
    layout.positions.reserve(itemWidths.size());
    for (const float width : itemWidths)
    {
        if (!firstOnLine)
        {
            if (x + gapX + width > lineMaxX + 0.5F)
            {
                x           = origin.x;
                y          += lineHeight + lineGap;
                firstOnLine = true;
            }
            else
            {
                x += gapX;
            }
        }
        layout.positions.emplace_back(x, y);
        x          += std::max(width, 0.0F);
        firstOnLine = false;
    }
    layout.height = itemWidths.empty() ? 0.0F : (y - origin.y) + lineHeight;
    return layout;
}

//!==========================================================================
//! Settings card: the rounded group container every section renders its rows
//! in. Neutral cards are the lightest surface over the page with a hairline
//! edge; tinted cards (e.g. the live-status strip) pass a container role and
//! drop the border. Rows are full-bleed inside it and stack edge to edge.
//!==========================================================================
struct SettingsCardScope
{
    bool                visible = false;
    ImGuiEx::StyleGuard styleGuard;

    explicit operator bool() const { return visible; }
};

inline auto BeginSettingsCard(
    const char                *strId,
    const ImGuiEx::M3::Spec::ColorRole fill     = ImGuiEx::M3::Spec::ColorRole::surfaceContainerLowest,
    const bool                bordered = true
) -> SettingsCardScope
{
    using namespace ImGuiEx::M3;
    auto      &m3Styles = Context::GetM3Styles();
    auto       guard    = ImGuiEx::StyleGuard()
                           .Color<ImGuiCol_ChildBg>(m3Styles.Colors()[fill])
                           .Color<ImGuiCol_Text>(m3Styles.Colors()[Spec::ColorRole::onSurface])
                           .Color<ImGuiCol_Border>(m3Styles.Colors()[Spec::ColorRole::outlineVariant])
                           .Style<ImGuiStyleVar_ChildRounding>(m3Styles.GetPixels(Spec::ShapeCorner::Medium))
                           .Style<ImGuiStyleVar_WindowPadding>(ImVec2(0.0F, 0.0F))
                           .Style<ImGuiStyleVar_ItemSpacing>(ImVec2(0.0F, 0.0F))
                           .Style<ImGuiStyleVar_ChildBorderSize>(bordered ? 1.0F : 0.0F);
    if (!ImGui::BeginChild(strId, {0.0F, 0.0F}, bordered ? ImGuiEx::ChildFlags().AutoResizeY().Borders() : ImGuiEx::ChildFlags().AutoResizeY()))
    {
        return {};
    }
    return {.visible = true, .styleGuard = std::move(guard)};
}

inline void EndSettingsCard()
{
#ifdef SIMPLEIME_PREVIEW
    if (std::getenv("PREVIEW_ROW_DEBUG") != nullptr)
    {
        ImGuiWindow *w = ImGui::GetCurrentWindow();
        std::fprintf(stderr, "CARD content=%.2f..%.2f pad=(%.1f,%.1f)\n", w->DC.CursorStartPos.y, w->DC.CursorMaxPos.y, w->WindowPadding.x, w->WindowPadding.y);
    }
#endif
    ImGui::EndChild();
}

//!==========================================================================
//! Settings row: one setting inside a card. Title (and optional supporting
//! copy, wrapped at a readable measure) sits left; the trailing control is
//! right-anchored by the row helpers. Interactive rows highlight on hover
//! and report a click of the row body, so a toggle's whole row is the hit
//! target — the checkbox visual itself owns no input.
//!==========================================================================
struct SettingsRowScope
{
    bool    visible       = false;
    bool    pressed       = false; ///< interactive rows: the row body was clicked this frame
    ImRect  bb{};
    float   contentX      = 0;   ///< left edge for leading content
    float   contentY      = 0;   ///< top edge for leading content
    float   textWidth     = 0;   ///< wrap width for title / supporting copy
    float   titleHeight   = 0;   ///< line boxes the title occupies (≥1); RowSupporting stacks below it
    float   trailingRight = 0;   ///< right edge for trailing controls
    float   centerY       = 0;   ///< vertical center of the row
    std::string_view title;    ///< the title the row was opened with; RowTitle(row) reuses it

    explicit operator bool() const { return visible; }
};

namespace Detail
{
//! imguiex keeps this helper file-local; panels need it for the disabled
//! rendering of hand-drawn row content.
inline auto IsItemDisabled() -> bool
{
    return (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
}

//! M3 disabled content: the scheme color knocked down to 38% alpha.
inline auto ContentColor(const ImGuiEx::M3::Spec::ColorRole role, const float alpha) -> ImVec4
{
    auto  &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    ImVec4 color    = m3Styles.Colors()[role];
    color.w = IsItemDisabled() ? alpha * 0.38F : alpha;
    return color;
}

//! A scheme color at an explicit alpha (disabled container fills, etc.).
inline auto AlphaColor(const ImGuiEx::M3::Spec::ColorRole role, const float alpha) -> ImVec4
{
    auto  &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    ImVec4 color    = m3Styles.Colors()[role];
    color.w = alpha;
    return color;
}

//! Role-styled text with a wrap width, immune to the disabled style-alpha so
//! the 38% content alpha above is the only dimming applied. Rendered through
//! the facade so wrapped lines advance by the M3 line box — ImGui's own
//! multi-line advance is one raw EM, which reads cramped for CJK copy.
inline void RoleText(const std::string_view text, const ImVec4 &color, const float wrapWidth)
{
    ImGuiEx::M3::TextUnformatted(text, color, wrapWidth > 0.0F ? ImGui::GetCursorPosX() + wrapWidth : -1.0F);
}
} // namespace Detail

//! Hover/press wash behind a hand-drawn widget: Pressed while held, Hovered
//! otherwise, under the widget's own background/content roles. Callers keep
//! the disabled + hover gating; this only draws the fill.
inline void DrawStateWash(
    ImDrawList                        *drawList,
    const ImRect                      &bb,
    const bool                         held,
    const ImGuiEx::M3::Spec::ColorRole bgRole,
    const ImGuiEx::M3::Spec::ColorRole contentRole,
    const float                        rounding,
    const ImDrawFlags                  corners = 0
)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    const auto wash = held ? m3Styles.Colors().Pressed(bgRole, contentRole)
                           : m3Styles.Colors().Hovered(bgRole, contentRole);
    drawList->AddRectFilled(bb.Min, bb.Max, ImGui::ColorConvertFloat4ToU32(wash), rounding, corners);
}

namespace
{
struct RowGeometry
{
    float titleLineHeight = 0.F;
    float titleHeight     = 0.F; ///< wrapped: a long title can span several line boxes
    float supportHeight   = 0.F;
};
} // namespace

inline auto BeginSettingsRowEx(
    const char               *strId,
    const std::string_view    title,
    const std::string_view    supporting,
    const float               trailingWidth,
    const float               controlHeight,
    const float               textMeasureCap,
    const bool                interactive
) -> SettingsRowScope
{
    using namespace ImGuiEx::M3;
    auto *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return {};
    }
    auto       &m3Styles    = Context::GetM3Styles();
    const float padX        = m3Styles.GetPixels(Spec::List::paddingX);
    const float padY        = m3Styles.GetPixels(Spec::List::paddingY);
    const float minContent  = m3Styles.GetPixels(Spec::List::minContentHeight);

    // Measure the copy up front: the row height commits before anything draws.
    RowGeometry geometry;
    float       textWidth = 0.0F;
    {
        const auto titleFont = m3Styles.UseTextRole<Spec::List::textRole>();
        geometry.titleLineHeight = m3Styles.GetLastText().currText.lineHeight;

        textWidth = ImGui::GetContentRegionAvail().x - padX * 2.0F - trailingWidth;
        if (textMeasureCap > 0.0F)
        {
            textWidth = std::min(textWidth, textMeasureCap);
        }
        textWidth = std::max(textWidth, ImGui::GetTextLineHeight() * 8.0F);

        // A title is not necessarily one line: long translations wrap at the
        // row's measure, and both the row height and RowSupporting's stack
        // position must follow the wrapped line count.
        geometry.titleHeight =
            title.empty() ? geometry.titleLineHeight : std::max(geometry.titleLineHeight, ImGuiEx::M3::MeasureWrappedText(title, textWidth).y);

        if (!supporting.empty())
        {
            const auto supportFont = m3Styles.UseTextRole<Spec::TextRole::BodySmall>();
            // Measure like RoleText renders: M3 line boxes, not ImGui's EM advance.
            geometry.supportHeight = ImGuiEx::M3::MeasureWrappedText(supporting, textWidth).y;
        }
    }

    const float contentH = std::max(minContent, geometry.titleHeight + (supporting.empty() ? 0.0F : geometry.supportHeight + m3Styles.GetPixels(Spec::dp<2>())));
    const float rowH     = std::max(contentH, controlHeight) + padY * 2.0F;

    SettingsRowScope row;
    row.bb            = ImRect(window->DC.CursorPos, {window->DC.CursorPos.x + ImGui::GetContentRegionAvail().x, window->DC.CursorPos.y + rowH});
    row.contentX      = row.bb.Min.x + padX;
    row.contentY      = row.bb.Min.y + padY;
    row.textWidth     = textWidth;
    row.titleHeight   = geometry.titleHeight;
    row.trailingRight = row.bb.Max.x - padX;
    row.centerY       = row.bb.GetCenter().y;
    row.title         = title;

    const ImGuiID id = interactive ? window->GetID(strId) : 0;
    ImGui::ItemSize(row.bb.GetSize());
    if (!ImGui::ItemAdd(row.bb, id))
    {
        return {};
    }
    row.visible = true;

    if (interactive)
    {
        bool  hovered = false;
        bool  held    = false;
        row.pressed   = ImGui::ButtonBehavior(row.bb, id, &hovered, &held);
        ImGui::RenderNavCursor(row.bb, id);
#ifdef SIMPLEIME_PREVIEW
        // Preview harness: force a row's hover state by ID for geometry
        // inspection without a physical mouse (PREVIEW_FORCE_ROW_HOVER=##Id).
        if (const char *needle = std::getenv("PREVIEW_FORCE_ROW_HOVER"); needle != nullptr && hovered == false)
        {
            if (std::strstr(strId, needle) != nullptr)
            {
                hovered = true;
            }
        }
#endif
        if (!Detail::IsItemDisabled() && (hovered || held))
        {
#ifdef SIMPLEIME_PREVIEW
            if (std::getenv("PREVIEW_ROW_DEBUG") != nullptr)
            {
                std::fprintf(
                    stderr,
                    "ROW %s bb=(%.2f,%.2f)-(%.2f,%.2f) card=(%.2f,%.2f)-(%.2f,%.2f) R=%.2f B=%.2f\n",
                    strId,
                    row.bb.Min.x,
                    row.bb.Min.y,
                    row.bb.Max.x,
                    row.bb.Max.y,
                    window->Pos.x,
                    window->Pos.y,
                    window->Pos.x + window->Size.x,
                    window->Pos.y + window->Size.y,
                    window->WindowRounding,
                    window->WindowBorderSize
                );
            }
#endif
            // The state layer is full-bleed, so its silhouette is the card's:
            // corners that coincide with the card's rounded corners take the
            // card rounding. A square band would paint past the card's corner
            // arcs, since content only clips to the child's square rect.
            const ImRect cardRect    = window->Rect();
            ImDrawFlags  cornerFlags = 0;
            if (row.bb.Min.y <= cardRect.Min.y + 0.5F)
            {
                cornerFlags |= ImDrawFlags_RoundCornersTop;
            }
            if (row.bb.Max.y >= cardRect.Max.y - 0.5F)
            {
                cornerFlags |= ImDrawFlags_RoundCornersBottom;
            }
            DrawStateWash(
                window->DrawList,
                row.bb,
                held,
                Spec::ColorRole::surfaceContainerHighest,
                Spec::ColorRole::onSurface,
                cornerFlags == 0 ? 0.0F : window->WindowRounding,
                cornerFlags
            );
            // The window outline is drawn before content, so an edge row's wash
            // covers the inner half of the outline arc; trace it again on top.
            if (cornerFlags != 0 && window->WindowBorderSize > 0.0F)
            {
                window->DrawList->AddRect(cardRect.Min, cardRect.Max, ImGui::GetColorU32(ImGuiCol_Border), window->WindowRounding, 0, window->WindowBorderSize);
            }
        }
    }

    ImGui::BeginGroup();
    ImGui::SetCursorScreenPos({row.contentX, row.contentY});
    // Establish the title line so SameLine content and AlignedLabel center on it.
    ImGui::Dummy({0.0F, geometry.titleLineHeight});
    ImGui::SameLine(0.0F, 0.0F);
    return row;
}

//! Interactive row: hover highlight + row-body clicks reported in `.pressed`.
//! Pass the title when one will be drawn via RowTitle so a wrapped title
//! grows the row; titles with a leading offset (swatches, dots) measure
//! differently — leave those empty and size the row via controlHeight.
inline auto BeginSettingsRow(
    const char            *strId,
    const std::string_view title         = {},
    const std::string_view supporting    = {},
    const float            trailingWidth = 0.0F,
    const float            controlHeight = 0.0F,
    const float            textMeasureCap = 0.0F
) -> SettingsRowScope
{
    return BeginSettingsRowEx(strId, title, supporting, trailingWidth, controlHeight, textMeasureCap, true);
}

//! Plain row: layout only, no hover, no row-level interaction.
inline auto BeginPlainSettingsRow(
    const std::string_view title          = {},
    const std::string_view supporting     = {},
    const float            trailingWidth  = 0.0F,
    const float            controlHeight  = 0.0F,
    const float            textMeasureCap = 0.0F
) -> SettingsRowScope
{
    return BeginSettingsRowEx(nullptr, title, supporting, trailingWidth, controlHeight, textMeasureCap, false);
}

inline void EndSettingsRow(const SettingsRowScope &row)
{
    if (!row)
    {
        return;
    }
    ImGui::EndGroup();
    // BeginGroup opens after the row's ItemSize has already advanced the
    // cursor one ItemSpacing past the row, so the group's implicit boundary
    // item lands below row.bb. Left as-is, the card's auto-fit grows one
    // spacing past the last row, leaving an empty strip under its state
    // layer; trim the content extent back to the row's real bottom.
    ImGui::GetCurrentWindow()->DC.CursorMaxPos.y = std::min(ImGui::GetCurrentWindow()->DC.CursorMaxPos.y, row.bb.Max.y);
    ImGui::SetCursorScreenPos({row.bb.Min.x, row.bb.Max.y});
}

//! Divider between contiguous interactive rows. M3::Divider() leaves its
//! trailing ItemSpacing below the line, so the next row — and its hover
//! state layer — started one spacing under the line, reading as a light
//! sliver the divider sat on. Pull the cursor back to the line's bottom
//! edge: the line is the row boundary itself, and each row's own padding
//! supplies the breathing room.
inline void RowDivider()
{
    auto *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return;
    }
    ImGuiEx::M3::Divider();
    ImGui::SetCursorScreenPos({ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y});
}

//! Row title in the list text role, at the content origin (offset for leading
//! widgets that already drew, e.g. a swatch or a dot).
inline void RowTitle(const SettingsRowScope &row, const std::string_view title, const float offsetX = 0.0F)
{
    using namespace ImGuiEx::M3;
    if (!row || title.empty())
    {
        return;
    }
    auto       &m3Styles = Context::GetM3Styles();
    const auto  fontScope = m3Styles.UseTextRole<Spec::List::textRole>();
    ImGui::SetCursorScreenPos({row.contentX + offsetX, row.contentY});
    // A leading widget wider than the row's text measure leaves no room: skip
    // the title rather than let it run into the trailing controls unwrapped.
    if (row.textWidth - offsetX > 0.0F)
    {
        Detail::RoleText(title, Detail::ContentColor(Spec::ColorRole::onSurface), row.textWidth - offsetX);
    }
}

//! Renders the title the row was opened with (same view the row measured).
//! Rows with a leading offset (swatches, dots) must still use the explicit
//! overload — their measured title width differs from the scope's.
inline void RowTitle(const SettingsRowScope &row, const float offsetX = 0.0F)
{
    RowTitle(row, row.title, offsetX);
}

//! Supporting copy under the title, BodySmall in the secondary tone, wrapped
//! at the row's measure. Stacks below the title's ACTUAL height (the row
//! measured it) — a wrapped title no longer collides with the copy.
inline void RowSupporting(const SettingsRowScope &row, const std::string_view text)
{
    using namespace ImGuiEx::M3;
    if (!row || text.empty())
    {
        return;
    }
    auto &m3Styles = Context::GetM3Styles();
    ImGui::SetCursorScreenPos({row.contentX, row.contentY + row.titleHeight + m3Styles.GetPixels(Spec::dp<2>())});
    const auto supportFont = m3Styles.UseTextRole<Spec::TextRole::BodySmall>();
    Detail::RoleText(text, Detail::ContentColor(Spec::ColorRole::onSurfaceVariant), row.textWidth);
}

//! Stateless trailing switch visual: the row's own click target handles the
//! toggle, so the track carries no input of its own.
inline void RowTrailingSwitch(const SettingsRowScope &row, const bool on)
{
    using namespace ImGuiEx::M3;
    if (!row)
    {
        return;
    }
    auto       &m3Styles = Context::GetM3Styles();
    auto       *drawList = ImGui::GetWindowDrawList();
    const float trackW   = m3Styles.GetPixels(Spec::dp<44>());
    const float trackH   = m3Styles.GetPixels(Spec::dp<24>());
    const float thumb    = m3Styles.GetPixels(Spec::dp<20>());
    const float inset    = (trackH - thumb) * 0.5F;
    const float rounding  = trackH * 0.5F;

    const ImVec2 trackMin(row.trailingRight - trackW, row.centerY - trackH * 0.5F);
    const ImVec2 trackMax(trackMin.x + trackW, trackMin.y + trackH);
    const float  thumbCenterY = trackMin.y + trackH * 0.5F;
    if (on)
    {
        drawList->AddRectFilled(trackMin, trackMax, ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(Spec::ColorRole::primary)), rounding);
        drawList->AddCircleFilled(
            ImVec2(trackMax.x - inset - thumb * 0.5F, thumbCenterY), thumb * 0.5F, ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(Spec::ColorRole::onPrimary))
        );
    }
    else
    {
        drawList->AddRectFilled(trackMin, trackMax, ImGui::ColorConvertFloat4ToU32(Detail::AlphaColor(Spec::ColorRole::surfaceContainerHighest, 1.0F)), rounding);
        drawList->AddRect(trackMin, trackMax, ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(Spec::ColorRole::outlineVariant)), rounding, 0, 1.0F);
        drawList->AddCircleFilled(
            ImVec2(trackMin.x + inset + thumb * 0.5F, thumbCenterY), thumb * 0.5F, ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(Spec::ColorRole::outline))
        );
    }
    ImGui::SetCursorScreenPos(trackMin);
    ImGui::Dummy({trackW, trackH});
}

//! Width the trailing switch occupies inside a row.
inline auto SwitchReserve() -> float
{
    using namespace ImGuiEx::M3;
    return Context::GetM3Styles().GetPixels(Spec::dp<44>()) + Context::GetM3Styles().GetPixels(Spec::dp<12>());
}

//! Whole-row toggle: a click anywhere in the row flips `value`.
inline bool SettingsToggleRow(
    const char            *strId,
    const std::string_view title,
    const std::string_view supporting,
    bool                  &value,
    const float            textMeasureCap = 0.0F
)
{
    using namespace ImGuiEx::M3;
    const auto  row  = BeginSettingsRow(strId, title, supporting, SwitchReserve(), Context::GetM3Styles().GetPixels(Spec::dp<24>()), textMeasureCap);
    if (row)
    {
        RowTitle(row, title);
        if (!supporting.empty())
        {
            RowSupporting(row, supporting);
        }
        RowTrailingSwitch(row, value);
        EndSettingsRow(row);
    }
    if (row.pressed)
    {
        value = !value;
    }
    return row.pressed;
}

//!==========================================================================
//! TextLink: a quiet text-only action ("关闭", "更改", "重置"). LabelLarge in
//! the given content role with a soft rounded hover/press wash behind it —
//! no chrome at rest, matching a plain text link in the layout.
//!==========================================================================
inline auto TextLinkWidth(const std::string_view text) -> float
{
    using namespace ImGuiEx::M3;
    auto                              &m3Styles = Context::GetM3Styles();
    const auto                         font     = m3Styles.UseTextRole<Spec::TextRole::LabelLarge>();
    const float                        padX     = m3Styles.GetPixels(Spec::dp<8>());
    return ImGui::CalcTextSize(ImGuiEx::TextStart(text), ImGuiEx::TextEnd(text)).x + padX * 2.0F;
}

//! Height of a TextLink hit target, so rows can reserve the right control size.
inline auto TextLinkHeight() -> float
{
    using namespace ImGuiEx::M3;
    auto       &m3Styles = Context::GetM3Styles();
    const auto font      = m3Styles.UseTextRole<Spec::TextRole::LabelLarge>();
    return std::max(m3Styles.GetLastText().currText.lineHeight + m3Styles.GetPixels(Spec::dp<12>()), m3Styles.GetPixels(Spec::dp<32>()));
}

inline bool TextLink(
    const char                        *strId,
    const std::string_view             text,
    const ImGuiEx::M3::Spec::ColorRole role   = ImGuiEx::M3::Spec::ColorRole::onSurface,
    const ImGuiEx::M3::Spec::ColorRole bgRole = ImGuiEx::M3::Spec::ColorRole::surfaceContainerHighest
)
{
    using namespace ImGuiEx::M3;
    auto *window = ImGui::GetCurrentWindow();
    if (window->SkipItems || text.empty())
    {
        return false;
    }
    auto &m3Styles = Context::GetM3Styles();

    ImVec2        textSize{};
    const float   padX       = m3Styles.GetPixels(Spec::dp<8>());
    {
        const auto font = m3Styles.UseTextRole<Spec::TextRole::LabelLarge>();
        textSize        = ImGui::CalcTextSize(ImGuiEx::TextStart(text), ImGuiEx::TextEnd(text));
    }

    const ImVec2 size(textSize.x + padX * 2.0F, TextLinkHeight());
    const ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y));
    const ImGuiID id = window->GetID(strId);
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, id))
    {
        return false;
    }
    bool        hovered = false;
    bool        held    = false;
    const bool  pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    ImGui::RenderNavCursor(bb, id);
    if (!Detail::IsItemDisabled() && (hovered || held))
    {
        DrawStateWash(window->DrawList, bb, held, bgRole, role, m3Styles.GetPixels(Spec::ShapeCorner::ExtraSmall));
    }
    window->DrawList->AddText(
        ImVec2(bb.Min.x + padX, bb.Min.y + ImGuiEx::M3::CenteredTextOffsetY(size.y)),
        ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(role)),
        text.data(),
        text.data() + text.size()
    );
    return pressed;
}

//! A quiet text action anchored to the page header's right edge ("警告", "帮助").
struct PageHeaderLink
{
    const char      *strId = nullptr;
    std::string_view label;
};

//! Page title and one-line orientation copy, kept above the grouped settings.
//! Optional quiet links ride the title line's right edge — page-wide support
//! actions that would otherwise crowd the content below. `pressedIndex` (when
//! given) receives the pressed link's index, or stays untouched / -1 initially.
inline void PageHeader(
    const std::string_view                title,
    const std::string_view                supporting,
    const std::span<const PageHeaderLink> links        = {},
    int                                  *pressedIndex = nullptr
)
{
    auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();

    if (pressedIndex != nullptr)
    {
        *pressedIndex = -1;
    }

    const float topPad   = m3Styles.GetPixels(M3Spec::dp<4>());
    const float linkGap  = m3Styles.GetPixels(M3Spec::dp<4>());
    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    const float availW   = ImGui::GetContentRegionAvail().x;

    float titleLineHeight = 0.0F;
    float linksWidth      = 0.0F;
    float linksHeight     = 0.0F;
    {
        const auto titleFont = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::TitleLarge>();
        titleLineHeight      = m3Styles.GetLastText().currText.lineHeight;
        for (const auto &link : links)
        {
            linksWidth += TextLinkWidth(link.label);
            linksHeight = std::max(linksHeight, TextLinkHeight());
        }
        if (!links.empty())
        {
            linksWidth += linkGap * static_cast<float>(links.size() - 1);
        }
    }

    ImGui::Dummy({0.F, topPad});
    {
        const auto titleFont = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::TitleLarge>();
        // Elide instead of colliding with the links under narrow windows.
        const float titleMaxW = links.empty() ? availW : std::max(availW - linksWidth - linkGap * 4.0F, ImGui::GetTextLineHeight() * 4.0F);
        const auto  elided    = ElideText(title, titleMaxW);
        const std::string_view shownTitle = elided.empty() ? title : std::string_view{elided};
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::TitleLarge>(shownTitle, ImGuiEx::M3::Spec::ColorRole::onSurface);
    }
    const ImVec2 belowTitle = ImGui::GetCursorScreenPos();

    if (!links.empty())
    {
        float x = topLeft.x + availW - linksWidth;
        ImGui::SetCursorScreenPos({x, topLeft.y + topPad + (titleLineHeight - linksHeight) * 0.5F});
        for (size_t i = 0; i < links.size(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0F, linkGap);
            }
            if (TextLink(links[i].strId, links[i].label) && pressedIndex != nullptr)
            {
                *pressedIndex = static_cast<int>(i);
            }
        }
        ImGui::SetCursorScreenPos(belowTitle);
    }

    if (!supporting.empty())
    {
        ImGui::Dummy({0.F, m3Styles.GetPixels(M3Spec::dp<4>())});
        // wrapWidth 0 = fold at the available width; the facade owns the wrap.
        ImGuiEx::M3::TextUnformatted<ImGuiEx::M3::Spec::TextRole::BodyMedium>(supporting, ImGuiEx::M3::Spec::ColorRole::onSurfaceVariant, 0.0F);
    }
    ImGui::Dummy({0.F, m3Styles.GetPixels(M3Spec::dp<4>())});
}

//! Right-aligned trailing text (list role), e.g. a status value. `rightOffset`
//! reserves room to its right for further trailing widgets (a button).
inline void RowTrailingText(const SettingsRowScope &row, const std::string_view text, const ImGuiEx::M3::Spec::ColorRole role, const float rightOffset = 0.0F)
{
    using namespace ImGuiEx::M3;
    if (!row || text.empty())
    {
        return;
    }
    auto       &m3Styles = Context::GetM3Styles();
    const auto  fontScope = m3Styles.UseTextRole<Spec::List::textRole>();
    const float lineHeight = m3Styles.GetLastText().currText.lineHeight;
    const float width    = ImGui::CalcTextSize(ImGuiEx::TextStart(text), ImGuiEx::TextEnd(text)).x;
    // Top of the line box: RoleText centers glyphs inside it, so this keeps
    // the value's optical middle on row.centerY.
    ImGui::SetCursorScreenPos({row.trailingRight - rightOffset - width, row.centerY - lineHeight * 0.5F});
    Detail::RoleText(text, Detail::ContentColor(role), -1.0F);
}

//! Height of the compact trailing dropdown button.
inline auto ComboButtonHeight() -> float
{
    using namespace ImGuiEx::M3;
    return Context::GetM3Styles().GetPixels(Spec::dp<40>());
}

//! Right-anchor a quiet TextLink to the row's trailing edge: vertically
//! centered on the row, right edge at trailingRight (rightOffset reserves room
//! for sibling trailing widgets to its right). Runs of several links and
//! stacked (second-line) rows position themselves and call TextLink directly.
inline bool RowTrailingTextLink(
    const SettingsRowScope            &row,
    const char                        *strId,
    const std::string_view             label,
    const float                        rightOffset = 0.0F,
    const ImGuiEx::M3::Spec::ColorRole role   = ImGuiEx::M3::Spec::ColorRole::onSurface,
    const ImGuiEx::M3::Spec::ColorRole bgRole = ImGuiEx::M3::Spec::ColorRole::surfaceContainerHighest
)
{
    if (!row)
    {
        return false;
    }
    ImGui::SetCursorScreenPos({row.trailingRight - rightOffset - TextLinkWidth(label), row.centerY - TextLinkHeight() * 0.5F});
    return TextLink(strId, label, role, bgRole);
}

//! Width a trailing combo will occupy for `preview` inside a row whose content
//! spans `contentWidth` px — the single source of truth shared with
//! BeginRowTrailingCombo, so rows can reserve the exact trailing space before
//! their extent commits and the title can never run under the chip.
inline auto RowTrailingComboWidth(const std::string_view preview, const float contentWidth) -> float
{
    using namespace ImGuiEx::M3;
    auto &m3Styles = Context::GetM3Styles();
    float width    = 0.0F;
    {
        const auto fontScope = m3Styles.UseTextRole<Spec::List::textRole>();
        width = ImGui::CalcTextSize(ImGuiEx::TextStart(preview), ImGuiEx::TextEnd(preview)).x +
                m3Styles.GetPixels(Spec::dp<12>()) * 2.0F + m3Styles.GetPixels(Spec::dp<8>()) +
                m3Styles.GetPixels(Spec::dp<10>());
    }
    return std::clamp(width, m3Styles.GetPixels(Spec::dp<140>()), contentWidth * 0.5F);
}

//!==========================================================================
//! Trailing dropdown: a compact outlined button (preview value + drop arrow)
//! that opens an M3 menu anchored under it. Deliberately not the M3 outlined
//! text field BeginCombo wraps: the field's floating label repeats the row
//! title and clips against the border under font scaling, and its 56dp
//! input height reads as an editable box rather than a picker.
//!==========================================================================
inline auto BeginRowTrailingCombo(
    const SettingsRowScope &row,
    const char             *strId,
    const std::string_view  preview
) -> bool
{
    using namespace ImGuiEx::M3;
    if (!row || preview.empty())
    {
        return false;
    }
    auto       &m3Styles = Context::GetM3Styles();
    auto       *window   = ImGui::GetCurrentWindow();
    auto       *drawList = ImGui::GetWindowDrawList();

    const float height    = ComboButtonHeight();
    const float padX      = m3Styles.GetPixels(Spec::dp<12>());
    const float arrowSize = m3Styles.GetPixels(Spec::dp<10>());

    const float width = RowTrailingComboWidth(preview, row.bb.Max.x - row.contentX);
    // The clamped chip must not spill its preview past the arrow: elide to the
    // inner text area, the opened menu discloses the full values.
    std::string elidedPreview;
    {
        const auto fontScope = m3Styles.UseTextRole<Spec::List::textRole>();
        elidedPreview        = ElideText(preview, width - padX * 2.0F - m3Styles.GetPixels(Spec::dp<8>()) - arrowSize);
    }
    const std::string_view shownPreview = elidedPreview.empty() ? preview : std::string_view{elidedPreview};

    const ImVec2  rectMin(row.trailingRight - width, row.centerY - height * 0.5F);
    const ImRect  bb(rectMin, {rectMin.x + width, rectMin.y + height});
    const ImGuiID id = window->GetID(strId);
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, id))
    {
        return false;
    }
    bool       hovered = false;
    bool       held    = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    ImGui::RenderNavCursor(bb, id);

    if (!Detail::IsItemDisabled() && (hovered || held))
    {
        DrawStateWash(drawList, bb, held, Spec::ColorRole::surfaceContainerHighest, Spec::ColorRole::onSurface, m3Styles.GetPixels(Spec::ShapeCorner::ExtraSmall));
    }
    drawList->AddRect(bb.Min, bb.Max, ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(Spec::ColorRole::outlineVariant)), m3Styles.GetPixels(Spec::ShapeCorner::ExtraSmall));

    {
        const auto  fontScope = m3Styles.UseTextRole<Spec::List::textRole>();
        drawList->AddText(
            {bb.Min.x + padX, row.centerY + ImGuiEx::M3::CenteredTextOffsetY(0.0F)},
            ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(Spec::ColorRole::onSurface)),
            ImGuiEx::TextStart(shownPreview),
            ImGuiEx::TextEnd(shownPreview)
        );
    }
    if (!elidedPreview.empty())
    {
        ImGuiEx::M3::SetItemToolTip(preview);
    }
    //! No drop-arrow glyph in the icon font, so draw the M3 arrow_drop_down
    //! triangle directly.
    const float arrowRight = bb.Max.x - padX;
    drawList->AddTriangleFilled(
        {arrowRight - arrowSize, row.centerY - arrowSize * 0.3F},
        {arrowRight, row.centerY - arrowSize * 0.3F},
        {arrowRight - arrowSize * 0.5F, row.centerY + arrowSize * 0.3F},
        ImGui::ColorConvertFloat4ToU32(Detail::ContentColor(Spec::ColorRole::onSurfaceVariant))
    );

    bool popupOpen = ImGui::IsPopupOpen(id, ImGuiPopupFlags_None);
    if (!popupOpen && pressed)
    {
        ImGui::OpenPopup(strId);
        popupOpen = true;
    }
    ImGuiEx::M3::MenuConfiguration config{};
    config.widthFitPreview = true;
    return popupOpen && ImGuiEx::M3::BeginMenu(strId, bb.Min, bb.Max, config);
}

//! Ends the combo popup (when open).
inline void EndRowTrailingCombo(const bool comboOpen)
{
    if (comboOpen)
    {
        ImGuiEx::M3::EndCombo();
    }
}
} // namespace Ime::UI::Panels
