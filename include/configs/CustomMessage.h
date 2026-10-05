#pragma once

namespace Ime
{
enum CustomMessage
{
    WM_CUSTOM = 0x7000,
    CM_IME_COMPOSITION = WM_CUSTOM + 3,
    CM_EXECUTE_TASK,
    CM_ABORT_IME,
};
} // namespace Ime
