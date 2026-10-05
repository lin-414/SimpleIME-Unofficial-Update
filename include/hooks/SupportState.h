#pragma once

namespace Hooks
{
/// Install-time outcome of an input bridge, latched when its Install() (or the
/// lazy retry in Tick) last ran. The settings UI reports it verbatim: bridges
/// read their config gate once at install (game thread, SKSE messaging), so a
/// switch flipped in the panel only persists the TOML value until the next
/// launch — the compatibility captions say so instead of pretending the hooks
/// moved.
enum class SupportState
{
    Pending,     ///< install has not run yet
    Off,         ///< disabled by the configuration latched at install time
    Standoff,    ///< a rival plugin owns the same surface; support stays off
    NotDetected, ///< the host DLL was not loaded
    Failed,      ///< detected but negotiation/initialization failed
    Active       ///< negotiated and live
};
} // namespace Hooks
