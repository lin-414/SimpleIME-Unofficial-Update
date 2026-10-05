# SimpleIME — Unofficial Update

An unofficial, community-maintained update of [cyfewlp/SimpleIME](https://github.com/cyfewlp/SimpleIME) —
a Skyrim SE/AE SKSE plugin that brings native IME support to the game, so Chinese, Japanese,
Korean and other multi-byte languages can be typed in the game console and every text field.

It continues development from upstream `v2.2.1`: the stuck-IME breakage around mod menus is
fixed (including upstream's known crash when switching windows via Win+Shift+S during CJK
composition), the TSF / focus / teardown paths are hardened against crashes and races, and
IME input is added for the mod UI frameworks upstream does not cover — **Meridian UI** (CEF)
and **SKSE Menu Framework** (ImGui) — plus **Prisma UI** avoidance so the two never fight.

**Current:** v3.0.0-beta · **Base:** upstream `v2.2.1` (`a2cd39f`) · **License:** MIT

Generic usage, configuration and build instructions are documented in the
[upstream README](https://github.com/cyfewlp/SimpleIME#readme) and the fully commented
[contrib/config/SimpleIME.toml](contrib/config/SimpleIME.toml). This document describes only
what this fork changes relative to upstream; per-version details live in
[PROGRESS.md](PROGRESS.md) (upstream's own history is in [CHANGELOG.md](CHANGELOG.md)).

## New: IME input for mod UI frameworks

Upstream types only into Scaleform — the game's own menus and console. Mods increasingly
render their text fields with web/ImGui frameworks that never touch the Scaleform menu
stack; three integrations close that gap. All of them negotiate *public* extension
interfaces only and are inert when the host DLL is absent.

- **Meridian UI (CEF) input** (`input.meridian_support`, default on). Meridian-based UIs
  (e.g. Tailor, SkipQuestNG) render through CEF, so Scaleform char-event injection never
  reaches their text fields. Two backends feed the same session machinery:
  **NirnLabUIPlatform** (primary) negotiates `NL::UI::IUIPlatformAPI` over SKSE messaging,
  tracks browsers through the public `AddOrGetBrowser` vtable slot and engages the IME on
  `SetBrowserFocused` events — covering the UIPlatform-based UIs the older interface cannot
  see; **Meridian.View/1** (fallback, MeridianUI.dll 1.0-era) hooks the public `TryFocus`
  slot. Committed text is delivered through an injected JS bridge
  (`document.execCommand('insertText')`), which emits the native `input` events the page
  listens for. Composition and candidates render as a floating panel inside the page,
  mirroring the ImGui candidate window's theme, scale, layout direction and configured font.
- **SKSE Menu Framework (ImGui) input** (`input.skse_menu_framework_support`, default on).
  SKSEMF-based mod menus draw their text fields with the ImGui embedded in
  SKSEMenuFramework.dll: those fields never raise the game's text-entry counter, so stock
  activation and char-event delivery both missed them. The bridge resolves SKSEMF's public
  exports (`RegisterEventPriority`, `igGetIO`, `ImGuiIO_AddInputCharacter`, …), polls ImGui's
  `WantTextInput` from a `kBeforeRender` callback, and on field focus acquires a *text-entry
  lease* — one call to the native `ControlMap::AllowTextInput`, which flows through
  SimpleIME's own detour and engages the IME exactly like a vanilla text entry (language bar
  included). Committed text is queued on the IME thread and injected with
  `ImGuiIO_AddInputCharacter` inside the framework's render callback, so the input queue is
  never raced, while an input-dispatch hook neutralizes the printable-ASCII CharEvents the
  engine keeps generating from DirectInput during composition (in English mode they pass
  through untouched, so plain typing still works). The lease is released when the field
  deactivates, when the framework render loop goes silent, on load transitions, and by the
  leak healer — the counter can never stick. Requires SKSEMenuFramework.dll ≥ 3.7; export
  surface, offsets and the callback technique adapted from
  [cashboxs/TMS_SIMEtoSKSEMF](https://github.com/cashboxs/TMS_SIMEtoSKSEMF) (MIT).
- **Prisma UI (Ultralight) avoidance** (`input.prisma_avoidance`, default on). Prisma-based
  UIs (e.g. Outfit Wheeler) handle IME input natively. SimpleIME negotiates Prisma's public
  `IVPrismaUI1` read-only (only `HasAnyActiveFocus()` is queried, never from a window
  procedure), listens for the `PrismaUI.ImeAssociation` handshake message, and stands the
  IME down while a Prisma UI owns the keyboard, so the two never fight. If PrismaUI.dll is
  present but its V1 API is unavailable, SimpleIME fails safe and stays out of the way
  entirely (update PrismaUI to restore SimpleIME input).

## Fixed relative to upstream

### Stuck IME around mod menus (v2.2.2)

The root cause was a **leaked text-entry counter**: mod menus call `AllowTextInput(true)`
without a matching `false` — sometimes landing *after* the menu-close event, where no
further event would ever repair it — and the leaked counter kept the IME, the language bar
and the pausing tool window on indefinitely (and blinded the repair, since SimpleIME's own
`ToolWindowMenu` sits on the menu stack whenever the bar is visible — a self-sustaining
stuck state). Counter leaks are now healed (event repair + a stability-grace poll), the
hook's cached count re-syncs, and `ToolWindowMenu` no longer counts as a counter owner.
On top of that:

- `AbortIme` actually cancels the composition (TSF `TerminateComposition`; IMM32
  `NI_COMPOSITIONSTR/CPS_CANCEL`, editor cleared first so no partial text commits) and drops
  the `IN_COMPOSING` / `IN_CAND_CHOOSING` state flags — previously it only returned focus,
  so the menu kept swallowing every keystroke after closing.
- `EnableIme(false)` returns Win32 keyboard focus to the game window — the enable path
  focused the hidden 0×0 `ImeWnd`, but the disable path never focused back, leaving `WM_CHAR`
  eaten by the invisible window.
- `keepImeOpen` no longer inverts disable requests (`keepImeOpen || enable` used to turn a
  `false` into `true`): disable is unconditional, so the Steam overlay and the "Enable Mod"
  checkbox truly disable, while text-entry closes may still keep the IME open per the
  setting.
- The text-entry counter hook seeds its baseline from the live counter at plugin load (a
  text entry already open at load used to swallow the first 1→0 transition); any menu close
  that leaves a stale nonzero counter re-syncs IME state (opt-in `FixInconsistentTextEntryCount`);
  and a failed state sync keeps its dirty flag so the next trigger retries instead of
  silently forgetting.
- Language-bar show/hide is driven from the single `ImeManager::EnableIme` funnel so every
  disable path hides the bar; a `WM_INPUTLANGCHANGE` watchdog re-asserts the English layout
  whenever the game thread drifts away while the IME is disabled; and the bar's pin button
  actually toggles (it previously could never unpin).

### Focus steal, crash & teardown hardening (v2.3.0 / v2.3.1)

- **Upstream's known crash — switching windows via Win+Shift+S during CJK composition** —
  is fixed: ImGui input-key clearing runs on the render thread instead of the IME thread, an
  aborted composition no longer injects its partial text mid focus transition, and the
  composition is terminated on the IME thread under our control. The v2.3.1 follow-up
  restores the key-up side: a key still held when the focus left (Ctrl + Alt-Tab was the
  visible case) no longer stays "down" in ImGui forever.
- Robustness audit (crash/UAF/hang class): the JamieMods `ErrorNotifier` deque is
  mutex-guarded — it was mutated from the IME thread while the render thread iterated it
  (use-after-free); TSF composition/UIElement sinks lock the editor and candidate state
  against the render thread's copies (with a document-lock predicate so sinks inside
  `OnLockGranted` do not self-deadlock); the IME thread shuts down in an orderly way
  (`WM_QUIT`, teardown on the thread owning the COM apartment, no cross-thread COM teardown
  in static destruction); the render thread is fenced off a tearing-down `ImeWnd` by an
  atomic flag; the cross-thread UI-scale / overlay-request fields are atomics; candidate
  page indexes reported by a TIP are bounds-checked and candidate selection no longer
  underflows; a malformed `shortcut` config no longer hangs the game at boot; and one leaked
  TSF reference per composition session is gone.
- `SimpleIMETest` configures and builds again (it had been unbuildable since the translator
  moved into the JamieMods submodule). The headless suite has since grown to 48 GoogleTest
  cases — config round-trip, Meridian + NirnLab ABI and version gates, session protocol and
  JSON-argument decoding, settings converter, SKSEMF session/ASCII-filter decision logic —
  plus a Node regression test for the embedded Meridian bridge JS
  (`node test/MeridianBridge.cjs`).

## Upgrades

### Rendering & DPI

- **DPI awareness** (`core.force_dpi_awareness`, default on): the game process is made
  per-monitor-v2 aware, so the settings UI is no longer stretched blurry by DWM on scaled
  displays (a DPI-unaware process gets bitmap-stretched at 125%+).
- **Font size semantics**: ImGui font sizes are interpreted as em — upstream used the
  asc+desc span, which renders CJK ~25% smaller than Latin at the same size and squeezes
  bottom strokes — and FreeType loads with `ForceAutoHint` so vcpkg's native hinting no
  longer clips CJK glyph bottoms.

### Settings & language bar UI

- **Default dark & light themes** — a full palette rework behind the existing
  source-color / dark-mode / contrast controls: warm-neutral surface ladder with a matte
  sage accent (design boards in [docs](docs/default-theme-dark.png)).
- **Settings window redesign** — streamed single-column layout with live status, plus new:
  a *Compatibility* card with per-bridge support state (active / pending restart / not
  detected / conflict with a mutually-exclusive plugin), an *Advanced* page with one-click
  diagnostics copy (versions, SKSE, DPI, TSF, bridge states, config validation), log-level
  hot-switching, error-toast duration presets, and "open config / log" links.
- **Language bar redesign** — grouped layout with separators, input-mode light, conversion
  mode, pin and settings entry; auto shows/hides with text entry.

### Localization

- Four new UI languages — **German, Japanese, Korean, Russian** — joining the bundled
  English and Chinese, so six complete translations ship in every package. More can be
  added by dropping a `translate_<name>.toml` into `interface/SimpleIME/`.

### Engine & build

- CommonLibSSE-NG submodule upgraded 4.10.0 → **10.1.0** with zero API adaptations.
- A D3D present hook drives the per-frame work (the engine stops calling the menu display
  path during Meridian sessions), and the first enable of an IME session falls back to the
  user's own non-English TIP when nothing was remembered.

## Compatibility

- **TMS_SIMEtoSKSEMF**: mutually exclusive on the SKSE Menu Framework path (both manage a
  text-entry lease and inject committed text into the framework's ImGui). SimpleIME disables
  its built-in SKSEMF input when that plugin is present; remove one of the two to avoid
  duplicated functionality.
- **MeridianUI / NirnLabUIPlatform / PrismaUI / SKSE Menu Framework**: supported through
  their *public* extension interfaces only; all integrations degrade to no-ops when the
  DLLs are absent, and UIPlatform API majors outside 1.x–3.x are refused with a log line
  instead of being hooked blind.

## Sync with upstream

```
git fetch upstream && git merge upstream/main
```

(the `upstream` remote should point to `https://github.com/cyfewlp/SimpleIME.git`)

## Credits & third-party

- [cyfewlp/SimpleIME](https://github.com/cyfewlp/SimpleIME) — the base project (MIT)
- [kkEngine/NirnLabUIPlatform](https://github.com/kkEngine/NirnLabUIPlatform) — vendored
  public API headers under `extern/NirnLabUIPlatform/` for the UIPlatform interface
  negotiation (MIT)
- [MeridianUI](https://www.nexusmods.com/skyrimspecialedition/mods/141552) and
  [PrismaUI](https://github.com/mlthethemol/PrismaUI) — vendored SDK headers under
  `extern/` for the public interface negotiation (their licenses ship in `third_party/`
  of every release package)
- [ocornut/imgui](https://github.com/ocornut/imgui),
  [alandtse/CommonLibVR](https://github.com/alandtse/CommonLibVR) (CommonLibSSE-NG) and the
  [JamieMods](https://github.com/lin-414/JamieMods) fork — git submodules

See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full list.

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 cyfewlp. Unofficial-update maintenance:
lin-414.
