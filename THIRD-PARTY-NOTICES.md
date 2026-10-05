# Third-party notices

This project incorporates or adapts the following third-party work. License texts marked
with a path are shipped in this repository and in the `third_party/` directory of every
release package.

## SimpleIME (upstream)

The base project this repository patches. MIT — copyright (c) 2026 cyfewlp.
<https://github.com/cyfewlp/SimpleIME>

## NirnLabUIPlatform (vendored SDK headers)

`extern/NirnLabUIPlatform/` (`API.h`, `IBrowser.h`, `JSTypes.h`, `Settings.h`,
`Version.h`) vendor the published `NL::UI::IUIPlatformAPI` / `NL::CEF::IBrowser`
extension headers, used to negotiate the interface over SKSE messaging and to
hook the public `AddOrGetBrowser`/`ReleaseBrowserHandle`/`SetBrowserFocused`
vtable slots. MIT — kkEngine, see `extern/NirnLabUIPlatform/LICENSE`.
<https://github.com/kkEngine/NirnLabUIPlatform>

## TMS_SIMEtoSKSEMF

The SKSE Menu Framework integration adapts its export surface, its render-event
callback technique (`RegisterEventPriority` + `ImGuiIO_AddInputCharacter` on the
framework's own render path), the text-entry lease via the native
`ControlMap::AllowTextInput`, and the raw-ASCII input-dispatch filter
(`src/hooks/SkseMenuFrameworkBridge.cpp`). MIT — cashboxs.
<https://github.com/cashboxs/TMS_SIMEtoSKSEMF>

## MeridianUI (vendored SDK headers)

`extern/MeridianUI/` (`ViewAPI.h`, `Settings.h`) vendor the published `Meridian.View/1`
extension header, used read-only to negotiate the public interface. MIT — see
`extern/MeridianUI/LICENSE`.

## PrismaUI (vendored SDK headers)

`extern/PrismaUI/PrismaUI_API.h` vendors the published `IVPrismaUI1` header, used
read-only (only `HasAnyActiveFocus()` is queried). License — see
`extern/PrismaUI/LICENSE.md`.

## Dependencies (git submodules, not redistributed here)

- [ocornut/imgui](https://github.com/ocornut/imgui) — MIT
- [alandtse/CommonLibVR](https://github.com/alandtse/CommonLibVR) (CommonLibSSE-NG) — MIT
- [JamieMods](https://github.com/lin-414/JamieMods) — fork of cyfewlp/JamieMods carrying
  the thread-safe `ErrorNotifier` fix (branch `fix/error-notifier-thread-safety`) — MIT

## Bundled assets

- [lucide](https://lucide.dev) icon font — ISC, see `assets/lucide/Lucide License`
- GoogleTest — BSD-3-Clause (test target only)
