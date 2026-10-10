@echo off
rem Build the SimpleIME settings-window design preview.
rem Standalone: does not touch the main build directory (no RC-patch loss).
rem FreeType build (matches the game's IMGUI_ENABLE_FREETYPE rasterizer),
rem linking the vcpkg-installed freetype from the main build tree.
setlocal
cd /d "%~dp0..\.."

if not exist tools\design-preview\build mkdir tools\design-preview\build

set FT_INC=build\vcpkg_modules\x64-windows-static-md\include
set FT_LIBS=build\vcpkg_modules\x64-windows-static-md\lib\freetype.lib build\vcpkg_modules\x64-windows-static-md\lib\libpng16.lib build\vcpkg_modules\x64-windows-static-md\lib\zlib.lib build\vcpkg_modules\x64-windows-static-md\lib\brotlidec.lib build\vcpkg_modules\x64-windows-static-md\lib\brotlicommon.lib build\vcpkg_modules\x64-windows-static-md\lib\bz2.lib

clang-cl /nologo /std:c++latest /EHsc /utf-8 /O2 /MD /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_USE_MATH_DEFINES ^
  /FItools\design-preview\preview_pch.h /DSIMPLEIME_PREVIEW /DIMGUI_ENABLE_FREETYPE /DIMGUI_USER_CONFIG=\"imconfig.h\" /I . ^
  /I tools\design-preview ^
  /I extern\imgui ^
  /I extern\imgui\backends ^
  /I extern\imgui\misc\freetype ^
  /I "%FT_INC%" ^
  /I extern\JamieMods\imguiex ^
  /I extern\JamieMods\common ^
  /I extern\JamieMods\extern\material-color-utilities ^
  /I include ^
  tools\design-preview\DesignPreviewMain.cpp ^
  tools\design-preview\MockPanels.cpp ^
  extern\imgui\imgui.cpp ^
  extern\imgui\imgui_draw.cpp ^
  extern\imgui\imgui_tables.cpp ^
  extern\imgui\imgui_widgets.cpp ^
  extern\imgui\misc\freetype\imgui_freetype.cpp ^
  extern\imgui\backends\imgui_impl_win32.cpp ^
  extern\imgui\backends\imgui_impl_dx11.cpp ^
  extern\JamieMods\imguiex\imguiex\imguiex_m3.cpp ^
  extern\JamieMods\imguiex\imguiex\m3.cpp ^
  extern\JamieMods\imguiex\imguiex\M3ThemeBuilder.cpp ^
  extern\JamieMods\imguiex\imguiex\imguiex_m3_slider.cpp ^
  extern\JamieMods\cmake\material-you\mcu_utils.cpp ^
  extern\JamieMods\extern\material-color-utilities\cpp\cam\cam.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\cam\hct.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\cam\hct_solver.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\contrast\contrast.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\dislike\dislike.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\dynamiccolor\dynamic_color.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\dynamiccolor\dynamic_scheme.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\dynamiccolor\material_dynamic_colors.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\palettes\tones.cc ^
  extern\JamieMods\extern\material-color-utilities\cpp\scheme\scheme_tonal_spot.cc ^
  /Fe:tools\design-preview\build\design-preview.exe ^
  /Fo:tools\design-preview\build\ ^
  /link d3d11.lib dxgi.lib %FT_LIBS%
endlocal
