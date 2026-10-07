//
// Created by jamie on 2026/1/30.
//

#define IMGUI_DEFINE_MATH_OPERATORS

#include "ui/ImeWindow.h"

#include <string_view>
#include <utility>

#include "ImeWnd.hpp"
#include "RE/GRectEx.h"
#include "RE/M/MenuCursor.h"
#include "WCharUtils.h"
#include "core/State.h"
#include "ime/ITextService.h"
#include "ime/ImeController.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imguiex/ImGuiEx.h"
#include "imguiex/imguiex_enum_wrap.h"
#include "imguiex/imguiex_m3.h"
#include "log.h"
#include "imguiex/m3/facade/base.h"
#include "imguiex/m3/spec/shapes.h"
#include "hooks/MeridianBridge.h"
#include "hooks/PrismaBridge.h"
#include "hooks/SkseMenuFrameworkBridge.h"
#include "utils/InputFocusAnchor.h"

namespace Ime
{

namespace
{
void DrawComposition(const CompositionInfo &compositionInfo)
{
    const auto             &documentText = compositionInfo.documentText;
    const std::wstring_view editorTextSv(documentText);

    bool drawStartToCaret = false;
    if (compositionInfo.caretPos > 0)
    {
        auto startToCaret = WCharUtils::ToString(editorTextSv.substr(0, compositionInfo.caretPos));
        if (startToCaret.empty())
        {
            // Conversion failed for a non-empty wstring segment; render a placeholder.
            startToCaret = "?";
        }
        ImGuiEx::M3::AlignedLabel(startToCaret);
        drawStartToCaret = true;
    }

    const auto &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    // caret
    if (fmod(ImGui::GetTime(), 1.2) <= 0.8) // NOLINT(*-magic-numbers)
    {
        ImGuiWindow *window = ImGui::GetCurrentWindow();
        ImRect       caretRect;
        if (drawStartToCaret)
        {
            const auto lastItemMin = ImGui::GetItemRectMin();
            const auto lastItemMax = ImGui::GetItemRectMax();
            caretRect.Min          = ImVec2(lastItemMax.x, lastItemMin.y);
            caretRect.Max          = ImVec2(lastItemMax.x, lastItemMax.y);
        }
        else
        {
            const auto &lineHeight = m3Styles.GetLastText().currText.lineHeight;
            const float offsetY    = ImGuiEx::M3::HalfDiff(window->DC.CurrLineSize.y, lineHeight);
            caretRect.Min          = ImVec2(window->DC.CursorPos.x, window->DC.CursorPos.y + offsetY);
            caretRect.Max          = ImVec2(caretRect.Min.x, caretRect.Min.y + lineHeight);
        }

        const auto caretColor = m3Styles.Colors()[ImGuiEx::M3::Spec::TextFieldCommon::CaretColor];
        window->DrawList->AddLine(caretRect.Min, caretRect.Max, ImGui::ColorConvertFloat4ToU32(caretColor), 1.0f);
    }

    if (compositionInfo.caretPos < documentText.size())
    {
        const auto caretToEnd = WCharUtils::ToString(editorTextSv.substr(compositionInfo.caretPos));
        if (!caretToEnd.empty())
        {
            ImGui::SameLine(0.F, 0.F);
            ImGuiEx::M3::AlignedLabel(caretToEnd);
        }
    }
}

//! The data layer delivers every candidate pre-formatted as "{n}. {word}"
//! (Imm32TextService::DoUpdateCandidateList / TextStore::DoUpdateUIElement).
//! The horizontal row renders the Win11-style look: a dim digit separated
//! from the word by a small fixed gap ("1 啊"), so the number has to be split
//! off the label here.
auto SplitCandidateNumber(const std::string &candidate) -> std::pair<std::string_view, std::string_view>
{
    const auto separator = candidate.find(". ");
    if (separator == std::string::npos)
    {
        return {{}, candidate};
    }
    return {std::string_view{candidate.data(), separator},
            std::string_view{candidate.data() + separator + 2, candidate.size() - separator - 2}};
}

//! Shared spine of the two candidate layouts: hit-test one row, paint the
//! selected/hover pill (selected = primary, hover = surfaceContainerHigh,
//! unselected rows transparent), then hand the pill rect to the layout's
//! label painter. True when the row was clicked.
template <typename PaintLabel>
auto DrawCandidatePill(const std::size_t index, const bool selected, const ImVec2 &itemSize, const float rounding, PaintLabel &&paintLabel) -> bool
{
    ImGui::PushID(static_cast<int>(index));
    const bool   pressed = ImGui::InvisibleButton("##Candidate", itemSize);
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    auto        &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
    auto        *drawList = ImGui::GetWindowDrawList();
    ImU32        backColor = 0;
    if (selected)
    {
        backColor = ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ImGuiEx::M3::Spec::ColorRole::primary]);
    }
    else if (ImGui::IsItemHovered())
    {
        backColor = ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[ImGuiEx::M3::Spec::ColorRole::surfaceContainerHigh]);
    }
    if (backColor != 0)
    {
        drawList->AddRectFilled(itemMin, itemMin + itemSize, backColor, rounding);
    }
    paintLabel(itemMin, selected);
    ImGui::PopID();
    return pressed;
}

//! Dispatch the candidate commit for a click (result logged, never thrown).
void CommitClickedCandidate(const std::size_t clicked, const std::size_t candidateCount)
{
    if (clicked < candidateCount)
    {
        if (const auto result = ImeController::GetInstance()->CommitCandidate(static_cast<DWORD>(clicked));
            !IImeModule::IsSuccess(result))
        {
            logger::error("Candidate commit was not dispatched ({})", IImeModule::IsFailed(result) ? "failed" : "disabled");
        }
    }
}

void DrawCandidates(const CandidateUi &candidateUi)
{
    using size_type = CandidateUi::size_type;

    if (const auto &candidateList = candidateUi.CandidateList(); !candidateList.empty())
    {
        auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
        const auto &colors   = m3Styles.Colors();
        const auto  textColor = [&colors](const ImGuiEx::M3::Spec::ColorRole role) {
            return ImGui::ColorConvertFloat4ToU32(colors[role]);
        };
        const float gap      = m3Styles.GetPixels(ImGuiEx::M3::Spec::dp<10>());
        const float numGap   = m3Styles.GetPixels(ImGuiEx::M3::Spec::dp<6>());
        const float padX     = m3Styles.GetPixels(ImGuiEx::M3::Spec::dp<8>());
        const float padY     = m3Styles.GetPixels(ImGuiEx::M3::Spec::dp<4>());
        const float rounding = m3Styles.GetPixels(ImGuiEx::M3::Spec::ShapeCorner::Small);

        size_type clicked = candidateList.size();
        for (size_type index = 0; const auto &candidate : candidateList)
        {
            const auto [number, word] = SplitCandidateNumber(candidate);

            // SameLine between items only: a trailing SameLine after the last
            // item advanced the cursor by one more spacing step, and the
            // auto-resized window kept that as blank space on the right.
            if (index > 0)
            {
                ImGui::SameLine(0.F, gap);
            }
            const ImVec2 numSize(ImGui::CalcTextSize(number.data(), number.data() + number.size()).x, ImGui::GetTextLineHeight());
            const ImVec2 wordSize(ImGui::CalcTextSize(word.data(), word.data() + word.size()).x, ImGui::GetTextLineHeight());
            const ImVec2 itemSize(numSize.x + numGap + wordSize.x + padX * 2.F, numSize.y + padY * 2.F);
            if (DrawCandidatePill(index, index == candidateUi.Selection(), itemSize, rounding, [&](const ImVec2 &itemMin, const bool selected) {
                    const auto numColor  = selected ? ImGuiEx::M3::Spec::ColorRole::onPrimary : ImGuiEx::M3::Spec::ColorRole::onSurfaceVariant;
                    const auto wordColor = selected ? ImGuiEx::M3::Spec::ColorRole::onPrimary : ImGuiEx::M3::Spec::ColorRole::onSurface;
                    // Bare AddText pins the line box to the pill top; CJK ink hangs
                    // ~0.36em below the optical middle, so use the shared optical
                    // centering offset (offset from the pill's top, padY included).
                    const ImVec2 numPos(itemMin.x + padX, itemMin.y + ImGuiEx::M3::CenteredTextOffsetY(itemSize.y));
                    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), numPos, textColor(numColor), number.data(), number.data() + number.size());
                    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {numPos.x + numSize.x + numGap, numPos.y}, textColor(wordColor), word.data(), word.data() + word.size());
                }))
            {
                clicked = index;
            }
            index++;
        }
        CommitClickedCandidate(clicked, candidateList.size());
    }
}

auto DrawVerticalCandidates(const CandidateUi &candidateUi) -> void
{
    using size_type = CandidateUi::size_type;

    if (const auto &candidateList = candidateUi.CandidateList(); !candidateList.empty())
    {
        auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
        const auto  textColor = [](const ImGuiEx::M3::Spec::ColorRole role) {
            return ImGui::ColorConvertFloat4ToU32(ImGuiEx::M3::Context::GetM3Styles().Colors()[role]);
        };
        // Same pill styling as the horizontal chip row (DrawCandidates):
        // selected = primary pill with onPrimary text, hover =
        // surfaceContainerHigh, unselected rows transparent on the window.
        // M3::MenuItem was replaced because its spec pins the selected
        // container to tertiaryContainer with surfaceContainerLow resting
        // fills, which made the vertical list a different color scheme from
        // the horizontal row. Vertical rows keep the menu-item geometry:
        // full-width 44dp rows with the full "{n}. {word}" label.
        const float padX      = m3Styles.GetPixels(ImGuiEx::M3::Spec::dp<12>());
        const float rowHeight = m3Styles.GetPixels(ImGuiEx::M3::Spec::dp<44>());
        const float rounding  = m3Styles.GetPixels(ImGuiEx::M3::Spec::ShapeCorner::Small);

        // All rows share the widest label's width so the list reads as one
        // full-width surface (and the auto-resized window fits the content).
        float maxWidth = 0.F;
        for (const auto &candidate : candidateList)
        {
            const float width = ImGui::CalcTextSize(candidate.data(), candidate.data() + candidate.size()).x;
            if (width > maxWidth)
            {
                maxWidth = width;
            }
        }
        const ImVec2 rowSize(maxWidth + padX * 2.F, rowHeight);

        size_type clicked = candidateList.size();
        for (size_type index = 0; const auto &candidate : candidateList)
        {
            if (DrawCandidatePill(index, index == candidateUi.Selection(), rowSize, rounding, [&](const ImVec2 &itemMin, const bool selected) {
                    const auto labelColor = selected ? ImGuiEx::M3::Spec::ColorRole::onPrimary : ImGuiEx::M3::Spec::ColorRole::onSurface;
                    // Same optical centering as the horizontal pill row: bare AddText
                    // pins the line box to the pill top and CJK ink hangs below the
                    // middle.
                    const ImVec2 textPos(itemMin.x + padX, itemMin.y + ImGuiEx::M3::CenteredTextOffsetY(rowSize.y));
                    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, textColor(labelColor), candidate.data(), candidate.data() + candidate.size());
                }))
            {
                clicked = index;
            }
            index++;
        }
        CommitClickedCandidate(clicked, candidateList.size());
    }
}

// Retest cadence for an unlocked caret anchor, in frames: ~80ms at 60fps — a
// fast self-heal that still bounds the Scaleform query cost when no caret
// exists at all.
constexpr int kCaretAnchorRetryInterval = 5;

// Vertical clearance from the field anchor to the candidate window: the
// anchor is the caret line's bottom inside the field (or the click point on
// the fallback path), so a small gap drops the window just below the field
// instead of covering its input row.
constexpr float kFieldAnchorOffsetY = 12.0F;

auto UpdateWindowPosByCursor(ImVec2 &windowPos) -> bool
{
    // io.MousePos is the live cursor whatever its source: imgui_manager feeds
    // it from MenuCursor while the CursorMenu owns the pointer and from a
    // client-mapped GetCursorPos otherwise. Reading MenuCursor's singleton
    // here instead would trust a position that goes stale the moment the
    // CursorMenu closes — the singleton always exists.
    const ImVec2 mousePos = ImGui::GetMousePos();
    if (!ImGui::IsMousePosValid(&mousePos))
    {
        return false;
    }
    windowPos = mousePos;
    return true;
}

auto UpdateImeWindowPosByCaret(ImVec2 &windowPos) -> bool
{
    // Framework (ImGui) fields have no Scaleform caret. Anchor at the field
    // position the bridge captured when the field gained focus — the engine
    // cursor was on the field at that moment, unlike at composition start
    // where the mouse may have wandered off. Nudge below the click point so
    // the window clears the input row it anchors to.
    if (Hooks::SkseMenuFrameworkBridge::SessionActive())
    {
        if (Hooks::SkseMenuFrameworkBridge::HasFieldAnchor())
        {
            Hooks::SkseMenuFrameworkBridge::GetFieldAnchor(windowPos.x, windowPos.y);
            windowPos.y += kFieldAnchorOffsetY;
            return true;
        }
        return false;
    }

    // Meridian pages are CEF — the bridge renders candidates into the page
    // itself, this window must not compete. Prisma fields are Ultralight and
    // have no Scaleform caret either, but the bridge tracks a field anchor
    // from the live cursor while the session is open (paused while a
    // composition shows) — anchor there instead of chasing the live cursor.
    // Returning false when no anchor exists yet leaves the position
    // untouched (no cursor fallback here) and keeps the anchor unlocked so
    // the retry cadence re-reads it once tracking has captured one.
    if (Hooks::MeridianBridge::HasFocus())
    {
        return false;
    }
    if (Hooks::PrismaBridge::ShouldRoute())
    {
        if (!Hooks::PrismaBridge::GetFieldAnchor(windowPos.x, windowPos.y))
        {
            return false;
        }
        windowPos.y += kFieldAnchorOffsetY;
        return true;
    }

    auto &instance = InputFocusAnchor::GetInstance();
    if (!instance.ComputeScreenMetrics())
    {
        return false;
    }
    const auto &bounds = instance.GetLastBounds();

    windowPos.x = bounds.left;
    windowPos.y = bounds.bottom;
    return true;
}

/**
 * @return true when the policy's anchor is settled for this attempt. A false
 * return (caret query failed) keeps the anchor unlocked so Draw keeps retrying.
 */
auto UpdateImeWindowPos(Settings::WindowPosUpdatePolicy policy, ImVec2 &windowPos) -> bool
{
    // Prisma fields have no Scaleform caret and must never fall back to the
    // live cursor — that is the "candidate box appears wherever the mouse is"
    // bug. The bridge's field anchor (tracked from the live cursor while the
    // session is open, refreshed on clicks ImeMenu observes) is the only
    // truthful anchor; while it is missing, the previous position stands
    // and the retry cadence re-reads it once tracking has captured one.
    if (Hooks::PrismaBridge::ShouldRoute())
    {
        return UpdateImeWindowPosByCaret(windowPos);
    }
    switch (policy)
    {
        case Settings::WindowPosUpdatePolicy::BASED_ON_CURSOR:
            return UpdateWindowPosByCursor(windowPos);
        case Settings::WindowPosUpdatePolicy::BASED_ON_CARET: {
            // The Scaleform caret query can lose the race against focus
            // propagation on the appearance frame, or find no field at all
            // (ImGui/SKSEMF text, console). Falling back to the menu cursor —
            // where the user just clicked — beats the (0,0) birth a failed
            // query used to leave behind.
            if (UpdateImeWindowPosByCaret(windowPos))
            {
                return true;
            }
            UpdateWindowPosByCursor(windowPos);
            return false;
        }
        default:;
    }
    return true;
}

/**
 * @brief Ensures the IME window is fully contained within the viewport and avoids the input area.
 * @details
 * **Implementation Logic:**
 * Replaces manual clamping with ImGui internal `FindBestWindowPosForPopupEx`. This leverages
 * engine-native logic to calculate the optimal position relative to the `avoidRect`
 * (the text input area) while respecting viewport boundaries.
 *
 * @note **IsNeedRelayout** already removed because `FindBestWindowPosForPopupEx` is fast and designed for this purpose,
 * eliminating the need for a separate relayout check. The function will always compute the best position, ensuring the
 * IME window is correctly placed without additional overhead.
 */
void ClampWindowToViewport(Settings::WindowPosUpdatePolicy policy, ImVec2 &pos, const ImVec2 &size)
{
    if (policy == Settings::WindowPosUpdatePolicy::NONE) return;

    // Simple viewport clamp + a one-shot vertical flip. FindBestWindowPosForPopupEx
    // (ComboBox policy) is actively wrong for a point anchor: it positions from
    // the avoid rect and ignores pos, and its sticky direction memory makes the
    // popup climb by one box height PER FRAME once it flipped (the Right branch
    // offsets above the avoid rect, which IS the previous pos) until it sticks
    // in the top-left corner — the "candidate window drifts upward" bug.
    // Both policies anchor at a point now, so the popup just hugs that point.
    const auto &viewport = ImGui::GetMainViewport();
    const ImRect viewPortRect(viewport->Pos, viewport->Pos + viewport->Size);
    if (size.x <= 0.0F || size.y <= 0.0F)
    {
        return; // no measured size yet (first frame) — nothing to clamp against
    }
    pos.x = ImClamp(pos.x, viewPortRect.Min.x, ImMax(viewPortRect.Min.x, viewPortRect.Max.x - size.x));
    if (pos.y + size.y > viewPortRect.Max.y && pos.y - size.y >= viewPortRect.Min.y)
    {
        pos.y -= size.y; // would hang past the bottom and fits above: flip once
    }
    pos.y = ImClamp(pos.y, viewPortRect.Min.y, ImMax(viewPortRect.Min.y, viewPortRect.Max.y - size.y));
}
} // namespace

void ImeWindow::Draw(const CompositionInfo &compositionInfo, const CandidateUi &candidateUi, const Settings &settings)
{
    const auto &state = Core::State::GetInstance();
    if (state.ImeDisabled() || !state.IsImeInputting() /* || !state.HasAny(State::KEYBOARD_OPEN, State::IME_OPEN)*/)
    {
        // No CloseCurrentPopup here: this is a plain window, not a popup — the
        // call was a leftover no-op (guarded by ImGui's empty popup stack).
        return;
    }
    // A captured Meridian session owns the composition/candidate display: the
    // bridge renders them into a panel inside the page itself. This ImGui
    // window draws into the game's swap chain, which Meridian's own CEF
    // composition overdraws — the candidate appeared blurred and flickering
    // underneath its UI. When the capture is lost (no-field), the bridge
    // reports false again and this window becomes the fallback surface.
    if (Hooks::MeridianBridge::OwnsCandidateUi())
    {
        return;
    }
    const auto currentFrame = ImGui::GetFrameCount();
    const bool caretPolicy  = settings.input.posUpdatePolicy == Settings::WindowPosUpdatePolicy::BASED_ON_CARET;
    if (currentFrame > m_lastShowFrame + 1)
    {
        const bool anchored   = UpdateImeWindowPos(settings.input.posUpdatePolicy, m_imePos);
        m_caretAnchorLocked   = !caretPolicy || anchored;
        m_nextCaretRetryFrame = currentFrame + kCaretAnchorRetryInterval;
        // Diagnostic edge trigger: fires exactly when the composition/candidate
        // window (re)appears after not being drawn. Correlating this with the
        // enable/disable / overlay logs pins down which state transition the
        // user-visible "IME still active after ESC" symptom belongs to.
        logger::info(
            "Candidate window became visible (composing={}, tsfFocus={}, overlayShowing={}, overlayPinned={}, caretAnchorLocked={}, imePos=({:.0f},{:.0f}))",
            state.IsImeInputting(),
            state.TsfFocus(),
            settings.runtimeData.overlayShowing,
            settings.runtimeData.overlayPinned,
            m_caretAnchorLocked,
            m_imePos.x,
            m_imePos.y
        );
    }
    else if (caretPolicy && !m_caretAnchorLocked && currentFrame >= m_nextCaretRetryFrame)
    {
        // The appearance-frame anchor failed (race, or a caret-less surface
        // keeps the session unlocked): poll for a real caret rect on a short
        // cadence. The position itself stays where the appearance frame put
        // it — deliberately NOT mouse-tracked, the box must not wander.
        if (UpdateImeWindowPosByCaret(m_imePos))
        {
            m_caretAnchorLocked = true;
        }
        m_nextCaretRetryFrame = currentFrame + kCaretAnchorRetryInterval;
    }
    m_lastShowFrame = currentFrame;
    ClampWindowToViewport(settings.input.posUpdatePolicy, m_imePos, m_imeSize);
    ImGui::SetNextWindowPos({m_imePos.x, m_imePos.y});
    constexpr auto flags = ImGuiEx::WindowFlags().NoDecoration().AlwaysAutoResize().NoFocusOnAppearing().NoSavedSettings().NoNav();
    // Compact layout: the M3 ListItem reserves 52dp per row (10dp vertical padding
    // + 32dp minimum content height), which left ~30dp of blank space above and
    // below both the composition line and the chip row — the window was mostly
    // padding. Draw the rows directly instead. Window padding is the corner-radius
    // zone (8dp) on all sides; the corner curve only reaches x=16dp exactly at the
    // window edge, so 8dp of padding keeps content and row backgrounds clear of
    // the rounding.
    auto           &m3Styles       = ImGuiEx::M3::Context::GetM3Styles();
    const auto     inset           = m3Styles.GetPixels(ImGuiEx::M3::Spec::ShapeCorner::Small);
    const auto     mainStyleGuard  = ImGuiEx::StyleGuard().Style<ImGuiStyleVar_WindowPadding>(ImVec2(inset, inset));

    // Why not use `BeginComboPopup`?
    // While the IME window's lifecycle resembles a Combo, `BeginComboPopup` is unsuitable because:
    // 1. **Focus/Closure Policy:** Popups automatically close on "click-outside," which conflicts
    // with IME persistence requirements during composition.
    // 2. **State Control:** IME visibility is strictly driven by `Core::State`, whereas Popups
    // rely on ImGui's internal `OpenPopup` stack, leading to state synchronization issues.
    if (ImGui::Begin("IME", nullptr, flags))
    {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

        // ListItem used to supply the List text role around the composition
        // line (caret metrics and label centering read it). Chip and MenuItem
        // set their own roles, so scoping it over the whole body is safe.
        const auto compositionTextRole = m3Styles.UseTextRole<ImGuiEx::M3::Spec::List::textRole>();
        DrawComposition(compositionInfo);
        ImGuiEx::M3::Divider();

        if (settings.appearance.verticalCandidateList)
        {
            DrawVerticalCandidates(candidateUi);
        }
        else
        {
            DrawCandidates(candidateUi);
        }

        m_imeSize = ImGui::GetWindowSize();
    }
    ImGui::End();
}
} // namespace Ime
