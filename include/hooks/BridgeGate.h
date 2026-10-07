#pragma once

#include "log.h"

#include <Windows.h>

namespace Hooks::BridgeGate
{
/// Config-switch gate: true when the bridge may install. On refusal logs
/// "<what> disabled by configuration"; the caller latches SupportState::Off
/// (each bridge owns its own state storage — atomic or not).
inline bool ConfigOpen(const bool enabled, const char *const what)
{
    if (enabled)
    {
        return true;
    }
    logger::info("{} disabled by configuration", what);
    return false;
}

/// Rival-bridge standoff: true when no rival input bridge for the same surface
/// is loaded. On a sighting logs the shared warning and returns false; the
/// caller latches SupportState::Standoff. `rivalName` is the log name (it may
/// differ from the file name, e.g. "Skyrim-Text-Bridge" vs SkyrimTextBridge.dll).
inline bool ClearOfRival(const wchar_t *const rivalDll, const char *const rivalName, const char *const what)
{
    if (GetModuleHandleW(rivalDll) == nullptr)
    {
        return true;
    }
    logger::warn("{} detected; {} stays off to avoid double text injection", rivalName, what);
    return false;
}
} // namespace Hooks::BridgeGate
