# Third-party notices

## This project

SimpleIME — Unofficial Update is licensed under **GNU GPL-3.0-or-later** — the full
license text ships as the repository's `LICENSE` file (and in every release package)
and at <https://www.gnu.org/licenses/>.

Copyright (C) 2026 lin-414. Based on SimpleIME by cyfewlp.

This program is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

The combined work is under the GPL because it links CommonLibSSE-NG
(GPL-3.0-or-later, see below) and vendors MeridianUI's GPL-3.0 header (see below).
MIT-licensed components below remain under their own terms; their copyright and
permission notices are retained here.

This project incorporates or adapts the following third-party work. License texts marked
with a path are shipped in this repository and in the `third_party/` directory of every
release package.

## SimpleIME (upstream)

The base project this repository patches. MIT — copyright (c) 2026 cyfewlp.
<https://github.com/cyfewlp/SimpleIME>

```
MIT License

Copyright (c) 2026 cyfewlp

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

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
extension header, used read-only to negotiate the public interface. GPL-3.0 (as
distributed upstream) — see `extern/MeridianUI/LICENSE`, which ships verbatim in every
release package as `third_party/MeridianUI-LICENSE`.

## PrismaUI (vendored SDK headers)

`extern/PrismaUI/PrismaUI_API.h` vendors the published `IVPrismaUI1` header, used
read-only (only `HasAnyActiveFocus()` is queried). License — see
`extern/PrismaUI/LICENSE.md`.

## Dependencies (git submodules, not redistributed here)

- [ocornut/imgui](https://github.com/ocornut/imgui) — MIT
- [alandtse/CommonLibVR](https://github.com/alandtse/CommonLibVR) (CommonLibSSE-NG) —
  **GPL-3.0-or-later** (relicensed upstream in `cf24ab8`, "Relicense to GPL-3.0-or-later";
  the full text is vendored unmodified as `extern/CommonLibSSE-NG/COPYING.txt`). SimpleIME
  links this library, which is what places the combined work under the GNU GPL.
- [JamieMods](https://github.com/lin-414/JamieMods) — fork of cyfewlp/JamieMods carrying
  the thread-safe `ErrorNotifier` fix (branch `fix/error-notifier-thread-safety`) — MIT

## Bundled assets

- [lucide](https://lucide.dev) icon font — ISC, see `assets/lucide/Lucide License`
- GoogleTest — BSD-3-Clause (test target only)
