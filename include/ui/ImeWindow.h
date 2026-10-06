//
// Created by jamie on 2026/1/30.
//

#pragma once

#include "Settings.h"
#include "imgui.h"

namespace ImGuiEx::M3
{
class M3Styles;
}

namespace Ime
{
struct CompositionInfo;
class CandidateUi;

class ImeWindow
{
    ImVec2 m_imePos;
    ImVec2 m_imeSize;
    int    m_lastShowFrame = -1;
    // BASED_ON_CARET anchor state: locked once a caret query succeeds for the
    // current appearance session; unlocked sessions retry on a short cadence
    // so a lost race at (re)appearance cannot strand the window off-caret.
    bool m_caretAnchorLocked   = false;
    int  m_nextCaretRetryFrame = 0;

public:
    void Draw(const CompositionInfo &compositionInfo, const CandidateUi &candidateUi, const Settings &settings);
};
} // namespace Ime
