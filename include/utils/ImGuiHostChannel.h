#pragma once

namespace Ime::Skyrim
{
/**
 * ## Why the channel is probed instead of configured
 *
 * A mod menu that draws with its own private Dear ImGui context gets its text
 * from one of two engine surfaces, and they are disjoint:
 *  - it registers a real `RE::IMenu` and forwards the Scaleform char events it
 *    receives to `io.AddInputCharacter` (ModExplorerMenu's `ModexGUIMenu`) — only
 *    the GFx commit route lands there;
 *  - it reads `RE::InputEvent`s off the engine's dispatcher (Tailor 3.x's
 *    `ImGuiHost::ProcessEvent`) — only the engine input-queue route lands there.
 *
 * One switch for both families means exactly one of the two mods can ever be
 * typed into, so `PollExternalImGuiSurface` asks the surface it just identified
 * which channel it actually consumes.
 *
 * What the ask can answer is "does this menu take Scaleform char events at all"
 * (its `IMenu::ProcessMessage` result); what it cannot answer is whether a menu
 * that takes them forwards them into its ImGui. Tailor's menu returns kHandled
 * for kScaleformEvent without ever reading the event, so hosts of that shape
 * carry a declared channel, keyed by their module file name — see the table in
 * `Utils.cpp`.
 */
enum class ImGuiHostChannel : int
{
    EngineEvents = 0, ///< deliver via BSInputEventQueue, with backspace erase for leaked preedit
    Scaleform    = 1, ///< deliver as GFx char events: a menu on the stack forwards them into ImGui
};

//! Game thread only: re-decide the channel of the current text target. Called
//! every frame next to `PollExternalImGuiSurface`; the probe only re-runs when
//! the topmost mod-owned menu changes.
void RefreshImGuiHostChannel();

//! Game thread: forget the cached answer, so the next surface is probed even if
//! its menu object lands on the freed address of the previous one.
void InvalidateImGuiHostChannel();

//! Any thread: the cached answer. Defaults to EngineEvents — the route the
//! feature shipped with — so a missing or inconclusive probe cannot re-break the
//! hosts that read the engine's input queue.
auto CurrentImGuiHostChannel() -> ImGuiHostChannel;
} // namespace Ime::Skyrim
