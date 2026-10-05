//
// Created by jamie on 2026/1/15.
//

#pragma once

#include "ui/fonts/FontBuilder.h"
#include "ui/fonts/preview_panel.h"

namespace Ime::UI
{
class FontBuilderPanel
{
    static constexpr auto TITLE_WARNING = "Warning";

public:
    explicit FontBuilderPanel() = default;

    void Draw(FontBuilder &fontBuilder, Settings &settings);

private:
    void DrawModals();

    FontPreviewPanel m_PreviewPanel{};
};
} // namespace Ime::UI
