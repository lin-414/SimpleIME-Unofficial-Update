//
// Created by jamie on 2026/3/10.
//

#include "ui/ImeOverlay.h"

#include "i18n/translator_manager.h"
#include "log.h"
#include "menu/MenuNames.h"
#include "path_utils.h"
#include "utils/Utils.h"

namespace Ime::UI
{
namespace
{
void TogglePinned(bool &pinned, bool &showing)
{
    if (pinned)
    {
        pinned = false;
    }
    else if (showing)
    {
        showing = false;
    }
    else
    {
        showing = true;
    }
}

/// 语言条刚出现后仍不接收鼠标的时间窗。见 ImeOverlay::Draw 里的说明。
constexpr unsigned long long kLanguageBarClickGraceMs = 300;

void HandleRequestAndSyncOverlayState(Settings::RuntimeData &runtimeData)
{
    if (!runtimeData.overlayShowing)
    {
        runtimeData.toolWindowShowing = false;
    }

    if (runtimeData.requestCloseTopWindow)
    {
        runtimeData.requestCloseTopWindow = false;
        if (runtimeData.toolWindowShowing)
        {
            runtimeData.toolWindowShowing = false;
        }
        else
        {
            // When there is no tool window to close, interpret "close top window" as a request
            // to close the overlay itself. This means pressing the close shortcut while the
            // settings/tool window is not open will close the entire overlay.
            runtimeData.overlayShowing = false;
        }
    }

    if (runtimeData.requestShowOverlay.exchange(false))
    {
        runtimeData.overlayShowing = true;
        logger::info("Language bar shown");
    }
    else if (runtimeData.requestHideOverlay.exchange(false))
    {
        if (runtimeData.overlayPinned)
        {
            logger::info("Language bar hide request ignored (pinned)");
        }
        else
        {
            runtimeData.overlayShowing = false;
            logger::info("Language bar hidden");
        }
    }
}
} // namespace

ImeOverlay::ImeOverlay(std::string_view language)
{
    i18n::SetTranslator(&m_translator);
    i18n::UpdateTranslator(language, "english", utils::GetPluginInterfaceDir());
}

ImeOverlay::~ImeOverlay()
{
    i18n::SetTranslator(nullptr);
}

auto ImeOverlay::Draw(const LangProfile &activeLangProfile, const std::vector<LangProfile> &langProfiles, Settings &settings, const bool shortcutPressed) -> void
{
    auto &runtimeData = settings.runtimeData;
    // The chord edge is evaluated once per frame in ImeWnd::Draw (swallow latch
    // and stuck-key heal applied there). Reading Settings::shortcut live keeps a
    // rebind in the Behaviour panel effective while this overlay stays alive.
    if (shortcutPressed)
    {
        TogglePinned(runtimeData.overlayPinned, runtimeData.overlayShowing);
    }

    // Handle before overlay render: avoid override the user request.
    HandleRequestAndSyncOverlayState(runtimeData);

    // The bar pops up right under the cursor (or the caret). A mod menu drawn with
    // its own Dear ImGui context cannot arbitrate that click with us — both contexts
    // see the same physical button — so for a moment after it appears the bar ignores
    // the mouse. UX only: the fatal pause came from the ToolWindow's own menu flags.
    if (runtimeData.overlayShowing && !m_overlayShowingLastFrame)
    {
        runtimeData.overlayShownAtMs = GetTickCount64();
    }
    m_overlayShowingLastFrame = runtimeData.overlayShowing;

    const auto notPinnedOverlay = !runtimeData.overlayPinned && runtimeData.overlayShowing;
    if (notPinnedOverlay)
    {
        if (m_toolWindow == nullptr)
        {
            m_toolWindow = std::make_unique<UI::ToolWindow>();
        }
        if (runtimeData.toolWindowShowing)
        {
            m_toolWindow->Draw(settings);
        }
        else
        {
            // Hidden but alive (e.g. Esc closed the settings window mid-capture:
            // the game's cancel event flips toolWindowShowing before the capture
            // widget could observe the key). A re-opened window must not resume
            // capturing.
            m_toolWindow->CancelShortcutCapture();
        }
    }
    else if (m_toolWindow != nullptr)
    {
        m_toolWindow.reset();
        // The tool window is being destroyed here (e.g. the user pinned the
        // language bar while the settings window was open) — its flag must go
        // with it. Leaving `toolWindowShowing` true made ImeMenu swallow ALL
        // Scaleform input game-wide and kept the MenuMode input context pushed
        // while the game was unpaused; only the overlay shortcut recovered.
        runtimeData.toolWindowShowing = false;
    }

    // Drawing after ToolWindow to avoid override the user `close top window` request: May reopen tool window.
    if (runtimeData.overlayShowing)
    {
        const bool interactive = GetTickCount64() - runtimeData.overlayShownAtMs >= kLanguageBarClickGraceMs;
        // May change runtimeData.toolWindowShowing, but apply it will be deferred to next frame.
        LanguageBar::Draw(runtimeData.overlayPinned, runtimeData.toolWindowShowing, activeLangProfile, langProfiles, interactive);
    }
}
} // namespace Ime::UI
