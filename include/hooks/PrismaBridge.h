#pragma once

#include "hooks/SupportState.h"

namespace Hooks::PrismaBridge
{
/// Query PrismaUI.dll's *public* V1 focus interface (read-only) so SimpleIME
/// can stand down while a Prisma UI owns the keyboard. Prisma-based UIs (e.g.
/// Outfit Wheeler) handle IME input natively — we must never fight them.
/// Safe to call when Prisma is not installed. Runs on the main thread
/// (SKSE kPostPostLoad).
void Install();
void Uninstall();

/// Install-time outcome (atomic; written by Install on the main thread,
/// readable from any thread). Backs the settings UI's compatibility caption.
[[nodiscard]] SupportState State();

/// Game-thread per-frame refresh of the cached focus state (call from
/// ImeMenu::PostDisplay). Throttled internally: HasAnyActiveFocus may wait on
/// Ultralight, so it must never be called from a window procedure.
void Refresh();

/// The game window received Prisma's `PrismaUI.ImeAssociation` registered
/// message: Prisma is associating its own IME context — latch "Prisma owns
/// input" until the next Refresh observes no active Prisma focus.
void OnAssociationMessage();

/// True while SimpleIME must not enable the IME (atomic; game thread writes,
/// IME thread reads).
[[nodiscard]] bool OwnsInput();

/// The registered window message id for Prisma's IME association handshake,
/// or 0 before Install.
[[nodiscard]] unsigned AssociationMessage();
} // namespace Hooks::PrismaBridge
