#pragma once

#include "hooks/SupportState.h"

#include <string>

namespace Hooks::NirnLabBridge
{
/// Negotiate the NirnLabUIPlatform API through SKSE messaging: register the
/// wildcard message listener and dispatch RequestVersion. Call once on the
/// main thread at SKSE kPostPostLoad (the documented negotiation point —
/// every UIPlatform consumer mod does the same). Safe to call when
/// NirnLabUIPlatform.dll is not installed at all (State() then reports
/// NotDetected).
void InstallMessaging();

/// Dispatch RequestAPI for the already version-checked protocol. Call on the
/// main thread at SKSE kInputLoaded (the API request initializes the host's
/// CEF stack, which the publisher documents to happen after input loads).
/// The synchronous response installs the focus observer hooks; after this
/// call UIPlatform browsers created by any consumer mod are tracked.
void RequestApi();

/// Install-time outcome of the UIPlatform backend (atomic; readable from any
/// thread). Backs the settings UI's compatibility caption.
[[nodiscard]] SupportState State();

/// Negotiated host library version ("3.3"), or "-" when not negotiated. For
/// the diagnostics clipboard only.
[[nodiscard]] std::string VersionDescription();
} // namespace Hooks::NirnLabBridge
