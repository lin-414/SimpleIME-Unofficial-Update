//
// Created by jamie on 2026/2/6.
//

#include "ui/LanguageBar.h"

#include "core/State.h"
#include "i18n/translator_manager.h"
#include "icons.h"
#include "ime/ImeController.h"
#include "imgui.h"
#include "imguiex/ImGuiEx.h"
#include "imguiex/imguiex_enum_wrap.h"
#include "imguiex/imguiex_m3.h"
#include "menu/MenuNames.h"
#include "tsf/ConversionModeUtil.h"
#include "tsf/LangProfile.h"
#include "ui/ToolWindow.h"

namespace Ime
{

namespace
{
constexpr auto LANGUAGE_BAR              = "LanguageBar";
constexpr auto LanguageProfilesMenuTitle = "##LanguageProfiles";

void DrawInputMethodsCombo(const LangProfile &activeLangProfile, const std::vector<LangProfile> &langProfiles)
{
    uint32_t clickedIndex = UINT32_MAX;
    uint32_t idx          = 0;
    if (ImGuiEx::M3::BeginMenu(LanguageProfilesMenuTitle, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()))
    {
        for (const auto &langProfile : langProfiles)
        {
            ImGui::PushID(static_cast<int>(idx));
            ImGuiEx::M3::MenuItemConfiguration config{};
            config.supportingText = langProfile.desc;
            const bool isSelected = IsEqualGUID(activeLangProfile.guidProfile, langProfile.guidProfile) == TRUE;
            if (ImGuiEx::M3::MenuItem(langProfile.localeDisplayName, isSelected, config) && !isSelected)
            {
                clickedIndex = idx;
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
            idx++;
        }
        ImGuiEx::M3::EndMenu();
    }
    if (clickedIndex != UINT32_MAX)
    {
        ImeController::GetInstance()->ActivateLangProfile(langProfiles[clickedIndex].guidProfile);
    }
}
} // namespace

namespace UI::LanguageBar
{
auto Draw(bool &pinned, bool &toolWindowShowing, const LangProfile &activeLangProfile, const std::vector<LangProfile> &langProfiles, bool interactive) -> void
{
    // NOTE: deliberately NOT NoInputs while pinned. ImGui only ever receives
    // mouse input while a menu context is active (ImeMenu forwards Scaleform
    // events), so the pinned bar cannot steal game clicks — but NoInputs made
    // the pin/unpin and settings buttons permanently unclickable, and the
    // "unpin from the button" escape hatch below never worked.
    // The builder's flag methods are consteval, so the whole chain has to be one
    // constant expression — pick the variant instead of OR-ing afterwards.
    const auto flags = interactive
                           ? ImGuiEx::WindowFlags().AlwaysAutoResize().NoNav().NoDecoration()
                           // Just appeared under the cursor: the click belongs to
                           // whatever is beneath us, not to the pin/settings buttons
                           // (see ImeOverlay::Draw).
                           : ImGuiEx::WindowFlags().AlwaysAutoResize().NoNav().NoDecoration().NoMouseInputs();
    if (ImGuiEx::M3::BeginFloatingToolbar(LANGUAGE_BAR, nullptr, M3Spec::ToolBarColors::Standard, flags))
    {
        auto       &m3Styles = ImGuiEx::M3::Context::GetM3Styles();
        constexpr auto iconButtonColors = ImGuiEx::M3::Spec::IconButtonColors::Standard;
        if (ImGuiEx::M3::SmallIconButton(pinned ? static_cast<std::string_view>(ICON_PIN_OFF) : ICON_PIN, iconButtonColors))
        {
            pinned = !pinned;
        }

        ImGui::SameLine();

        if (ImGuiEx::M3::SmallIconButton(ICON_SETTINGS, iconButtonColors))
        {
            toolWindowShowing = true;
        }
        ImGuiEx::M3::SetItemToolTip(Translate("Settings.Settings"));

        //! Hairline rule splitting the [pin | settings] action group from the
        //! language status section. Painted directly on the draw list: a Dummy
        //! item here would replace the carried line height and un-center every
        //! SameLine()'d item after it.
        {
            const float barH = m3Styles.GetPixels(M3Spec::ToolBarSizing<M3Spec::ToolBarVariant::Floating>::HorizontalContainerHeight);
            const float ruleH = m3Styles.GetPixels(M3Spec::dp<24>());
            const float gap   = m3Styles.GetPixels(M3Spec::dp<14>());
            ImGui::SameLine(0.0F, gap);
            const auto  lineTop = ImGui::GetCursorScreenPos().y;
            const float ruleX   = ImGui::GetCursorScreenPos().x;
            ImGui::GetWindowDrawList()->AddLine(
                {ruleX, lineTop + (barH - ruleH) * 0.5F},
                {ruleX, lineTop + (barH + ruleH) * 0.5F},
                ImGui::ColorConvertFloat4ToU32(m3Styles.Colors()[M3Spec::ColorRole::outlineVariant]),
                1.0F
            );
            // No item was submitted for the rule, so this hop measures its
            // spacing from the settings button again: gap + rule + gap.
            ImGui::SameLine(0.0F, gap * 2.0F + 1.0F);
        }

        auto        &state          = Core::State::GetInstance();
        const auto  &conversionMode = state.GetConversionMode();
        const auto   cModeName      = GetConversionModeNameShort(activeLangProfile.langid, conversionMode, state.IsKeyboardOpen());
        if (!cModeName.empty())
        {
            ImGuiEx::M3::AlignedLabel(cModeName);
            ImGuiEx::M3::SameLine(0.F, M3Spec::dp<8>());
        }

        // The toolbar itself is the container: the language entry renders
        // transparent at rest (hover and press supply the state layers) with
        // its menu-grade leading space collapsed, so the mode label and the
        // language name read as one status run instead of a boxed control.
        ImGuiEx::M3::MenuItemConfiguration config{};
        config.supportingText            = activeLangProfile.desc;
        config.leadingSpace              = 4.0F;
        config.trailingSpace             = 8.0F;
        config.transparentContainer      = true;
        if (ImGuiEx::M3::MenuItem(activeLangProfile.language, false, config))
        {
            ImGui::OpenPopup(LanguageProfilesMenuTitle);
        }
        DrawInputMethodsCombo(activeLangProfile, langProfiles);

        ImGuiEx::M3::EndFloatingToolbar();
    }
}
} // namespace UI::LanguageBar
} // namespace Ime
