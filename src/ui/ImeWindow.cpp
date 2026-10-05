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
        auto       *drawList = ImGui::GetWindowDrawList();

        size_type clicked = candidateList.size();
        for (size_type index = 0; const auto &candidate : candidateList)
        {
            const auto [number, word] = SplitCandidateNumber(candidate);

            ImGui::PushID(static_cast<int>(index));
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
            if (ImGui::InvisibleButton("##Candidate", itemSize))
            {
                clicked = index;
            }
            const ImVec2  itemMin   = ImGui::GetItemRectMin();
            const bool    selected  = index == candidateUi.Selection();
            ImU32         backColor = 0;
            if (selected)
            {
                backColor = textColor(ImGuiEx::M3::Spec::ColorRole::primary);
            }
            else if (ImGui::IsItemHovered())
            {
                backColor = textColor(ImGuiEx::M3::Spec::ColorRole::surfaceContainerHigh);
            }
            if (backColor != 0)
            {
                drawList->AddRectFilled(itemMin, itemMin + itemSize, backColor, rounding);
            }
            const auto numColor = selected ? ImGuiEx::M3::Spec::ColorRole::onPrimary : ImGuiEx::M3::Spec::ColorRole::onSurfaceVariant;
            const auto wordColor = selected ? ImGuiEx::M3::Spec::ColorRole::onPrimary : ImGuiEx::M3::Spec::ColorRole::onSurface;
            // Bare AddText pins the line box to the pill top; CJK ink hangs
            // ~0.36em below the optical middle, so use the shared optical
            // centering offset (offset from the pill's top, padY included).
            const ImVec2 numPos(itemMin.x + padX, itemMin.y + ImGuiEx::M3::CenteredTextOffsetY(itemSize.y));
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), numPos, textColor(numColor), number.data(), number.data() + number.size());
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {numPos.x + numSize.x + numGap, numPos.y}, textColor(wordColor), word.data(), word.data() + word.size());
            ImGui::PopID();
            index++;
        }
        if (clicked < candidateList.size())
        {
            ImeController::GetInstance()->CommitCandidate(static_cast<DWORD>(clicked));
        }
    }
}

auto DrawVerticalCandidates(const CandidateUi &candidateUi) -> void
{
    using size_type = CandidateUi::size_type;

    if (const auto &candidateList = candidateUi.CandidateList(); !candidateList.empty())
    {
        auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
        const auto &colors   = m3Styles.Colors();
        const auto  textColor = [&colors](const ImGuiEx::M3::Spec::ColorRole role) {
            return ImGui::ColorConvertFloat4ToU32(colors[role]);
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
        auto       *drawList  = ImGui::GetWindowDrawList();

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
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::InvisibleButton("##Candidate", rowSize))
            {
                clicked = index;
            }
            const ImVec2  itemMin   = ImGui::GetItemRectMin();
            const bool    selected  = index == candidateUi.Selection();
            ImU32         backColor = 0;
            if (selected)
            {
                backColor = textColor(ImGuiEx::M3::Spec::ColorRole::primary);
            }
            else if (ImGui::IsItemHovered())
            {
                backColor = textColor(ImGuiEx::M3::Spec::ColorRole::surfaceContainerHigh);
            }
            if (backColor != 0)
            {
                drawList->AddRectFilled(itemMin, itemMin + rowSize, backColor, rounding);
            }
            const auto labelColor = selected ? ImGuiEx::M3::Spec::ColorRole::onPrimary : ImGuiEx::M3::Spec::ColorRole::onSurface;
            // Same optical centering as the horizontal pill row: bare AddText
            // pins the line box to the pill top and CJK ink hangs below the
            // middle.
            const ImVec2 textPos(itemMin.x + padX, itemMin.y + ImGuiEx::M3::CenteredTextOffsetY(rowSize.y));
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, textColor(labelColor), candidate.data(), candidate.data() + candidate.size());
            ImGui::PopID();
            index++;
        }
        if (clicked < candidateList.size())
        {
            ImeController::GetInstance()->CommitCandidate(static_cast<DWORD>(clicked));
        }
    }
}

auto UpdateImeWindowPosByCaret(ImVec2 &windowPos) -> void
{
    auto &instance = InputFocusAnchor::GetInstance();
    instance.ComputeScreenMetrics();
    const auto &bounds = instance.GetLastBounds();

    windowPos.x = bounds.left;
    windowPos.y = bounds.bottom;
}

auto UpdateImeWindowPos(Settings::WindowPosUpdatePolicy policy, ImVec2 &windowPos) -> void
{
    switch (policy)
    {
        case Settings::WindowPosUpdatePolicy::BASED_ON_CURSOR:
            if (const auto *cursor = RE::MenuCursor::GetSingleton(); cursor != nullptr)
            {
                windowPos.x = cursor->cursorPosX;
                windowPos.y = cursor->cursorPosY;
            }
            break;
        case Settings::WindowPosUpdatePolicy::BASED_ON_CARET: {
            UpdateImeWindowPosByCaret(windowPos);
            break;
        }
        default:;
    }
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
void ClampWindowToViewport(Settings::WindowPosUpdatePolicy policy, ImVec2 &pos, const ImVec2 &size, ImGuiDir &lastAutoPosDir)
{
    if (policy == Settings::WindowPosUpdatePolicy::NONE) return;

    const auto &viewport = ImGui::GetMainViewport();
    ImRect      avoidRect;
    if (policy == Settings::WindowPosUpdatePolicy::BASED_ON_CURSOR)
    {
        avoidRect.Min = pos;
        avoidRect.Max = pos;
    }
    else
    {
        const auto &bounds = InputFocusAnchor::GetInstance().GetLastBounds();
        avoidRect.Min.x    = bounds.left;
        avoidRect.Min.y    = bounds.top;
        avoidRect.Max.x    = bounds.right;
        avoidRect.Max.y    = bounds.bottom;
    }
    const ImRect viewPortRect(viewport->Pos, viewport->Pos + viewport->Size);
    pos = ImGui::FindBestWindowPosForPopupEx(pos, size, &lastAutoPosDir, viewPortRect, avoidRect, ImGuiPopupPositionPolicy_ComboBox);
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
    if (currentFrame > m_lastShowFrame + 1)
    {
        UpdateImeWindowPos(settings.input.posUpdatePolicy, m_imePos);
        // Diagnostic edge trigger: fires exactly when the composition/candidate
        // window (re)appears after not being drawn. Correlating this with the
        // enable/disable / overlay logs pins down which state transition the
        // user-visible "IME still active after ESC" symptom belongs to.
        logger::info(
            "Candidate window became visible (composing={}, tsfFocus={}, overlayShowing={}, overlayPinned={})",
            state.IsImeInputting(),
            state.TsfFocus(),
            settings.runtimeData.overlayShowing,
            settings.runtimeData.overlayPinned
        );
    }
    m_lastShowFrame = currentFrame;
    ClampWindowToViewport(settings.input.posUpdatePolicy, m_imePos, m_imeSize, m_lastAutoPosDir);
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
