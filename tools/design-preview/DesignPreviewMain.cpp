//
// SimpleIME settings-window design preview.
//
// A standalone Win32 + DX11 + ImGui host that renders the redesigned
// ToolWindow with stubbed game dependencies, so the layout can be reviewed
// and screenshotted without launching Skyrim.
//
// Build: see tools/design-preview/build-preview.cmd
// Run:   design-preview.exe [path\to\lucide-icons.ttf]
//

#include <filesystem>
#include <map>
#include <string>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h" // PREVIEW_DUMP_TOOLTIP window internals
#include "imgui_freetype.h"
// NOTE: imguiex (and its dp<> spec) must be parsed before any Windows header —
// the SDK leaks a BASE_UNIT-like function-like macro that collides with it.
#include "imguiex/imguiex_m3.h"

#include <d3d11.h>
#include <windows.h>
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace mock
{
enum class Menu : int { InputStatus, Display, FontBuilder, Advanced };
extern Menu g_initialTab;      // MockPanels.cpp — set from --tab=
extern bool g_verticalPreview; // MockPanels.cpp — set from --vertical
extern float g_scrollTo;       // MockPanels.cpp — set from --scroll=
extern float g_windowWidth;    // MockPanels.cpp — set from --width=
}
namespace
{
float g_hoverX = -1.0F;        // --hover=X,Y pins the ImGui mouse for hover-state captures
float g_hoverY = -1.0F;
float g_uiScale = 0.0F;        // --scale=N applies the M3 type/spacing scale (game uses the DPI scale, e.g. 1.25)
}
extern void DrawMockToolWindow(); // MockPanels.cpp
extern void DrawMockLanguageBar(); // MockPanels.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// imgui_manager.cpp is not linked into the preview; provide the one symbol
// M3::Initialize needs (adds a separate, non-merged font — same call as in game).
namespace ImGuiEx
{
auto AddFont(const std::string &filePath) -> ImFont *
{
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(filePath.c_str());
}
} // namespace ImGuiEx

namespace
{
constexpr wchar_t WINDOW_CLASS[] = L"SimpleIMEDesignPreview";
constexpr wchar_t WINDOW_TITLE[] = L"SimpleIME Settings — Design Preview";

ID3D11Device           *g_device          = nullptr;
ID3D11DeviceContext    *g_context         = nullptr;
IDXGISwapChain         *g_swapChain       = nullptr;
ID3D11RenderTargetView *g_renderTarget    = nullptr;

auto CreateDeviceD3D(HWND hWnd) -> bool
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count                   = 1;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount                        = 2;
    sd.OutputWindow                       = hWnd;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags                              = 0;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL           featureLevel;
    return SUCCEEDED(
        D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device,
            &featureLevel, &g_context
        )
    );
}

void CreateRenderTarget()
{
    ID3D11Texture2D *backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer != nullptr)
    {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_renderTarget != nullptr)
    {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swapChain != nullptr) g_swapChain->Release();
    if (g_context != nullptr) g_context->Release();
    if (g_device != nullptr) g_device->Release();
    g_swapChain = nullptr;
    g_context   = nullptr;
    g_device    = nullptr;
}

auto WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) != 0)
    {
        return 1;
    }
    switch (msg)
    {
        case WM_SIZE:
            if (g_device != nullptr && wParam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

auto FindIconFontPath(int argc, char **argv) -> std::filesystem::path
{
    // A non-flag argument may be an explicit icon font path.
    for (int i = 1; i < argc; ++i)
    {
        if (const std::string_view arg = argv[i]; !arg.empty() && arg[0] != '-')
        {
            return arg;
        }
    }
    // Default: the shipped icon font in dist, relative to the exe directory.
    std::filesystem::path exeDir;
    wchar_t               modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
    {
        exeDir = std::filesystem::path(modulePath).parent_path();
    }
    for (const auto &candidate : {
             exeDir / L"../../../dist/SimpleIME/interface/SimpleIME/lucide-icons.ttf",
             exeDir / L"lucide-icons.ttf"
         })
    {
        if (exists(candidate))
        {
            return std::filesystem::absolute(candidate);
        }
    }
    return L"lucide-icons.ttf";
}
} // namespace

int main(int argc, char **argv)
{
    // Physical-pixel rendering: logical == physical, so screen captures of the
    // host window are 1:1 regardless of the launcher's DPI awareness.
    ImGui_ImplWin32_EnableDpiAwareness();

    for (int i = 1; i < argc; ++i)
    {
        if (const std::string_view arg = argv[i]; arg.starts_with("--tab="))
        {
            const auto value = arg.substr(6);
            if (value == "input") mock::g_initialTab = mock::Menu::InputStatus;
            else if (value == "appearance") mock::g_initialTab = mock::Menu::Display;
            else if (value == "fontbuilder") mock::g_initialTab = mock::Menu::FontBuilder;
            else if (value == "advanced") mock::g_initialTab = mock::Menu::Advanced;
        }
        else if (std::string_view(argv[i]) == "--vertical")
        {
            mock::g_verticalPreview = true;
        }
        else if (arg.starts_with("--scroll="))
        {
            mock::g_scrollTo = std::strtof(argv[i] + 9, nullptr);
        }
        else if (arg.starts_with("--width="))
        {
            mock::g_windowWidth = std::strtof(argv[i] + 8, nullptr);
        }
    else if (arg.starts_with("--hover="))
    {
        const char *pos = argv[i] + 8;
        g_hoverX        = std::strtof(pos, nullptr);
        if (const char *comma = std::strchr(pos, ','); comma != nullptr)
        {
            g_hoverY = std::strtof(comma + 1, nullptr);
        }
    }
    else if (arg.starts_with("--scale="))
    {
        g_uiScale = std::strtof(argv[i] + 8, nullptr);
    }
    }

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, WINDOW_CLASS, nullptr};
    RegisterClassExW(&wc);

    // A resizable host for both compact and expanded settings layouts.
    HWND hwnd = CreateWindowW(
        WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW, 20, 20, 1280, 920, nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!CreateDeviceD3D(hwnd))
    {
        return 1;
    }
    CreateRenderTarget();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Avoid stale preview window sizes after layout changes.

    // Base font stack mirrors the game's ImeApp::GetDefaultFontFilePathList:
    // system message font (Microsoft YaHei here) as primary, then Segoe UI
    // Emoji, Korean Malgun, and Japanese MS Gothic merged as supplementary.
    // CJK glyphs rasterize on demand (ImGui 1.92 dynamic fonts).
    ImFontConfig fontCfg;
    // Match ImeApp: msyh's native hinting compresses CJK glyph bottoms into
    // invisibility at UI sizes; the FreeType autohinter keeps them intact.
    io.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_ForceAutoHint;
    auto        &fonts = *io.Fonts;
    fonts.AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 0.0F, &fontCfg);
    fontCfg.MergeMode = true;
    fonts.AddFontFromFileTTF("C:/Windows/Fonts/seguiemj.ttf", 0.0F, &fontCfg);
    fonts.AddFontFromFileTTF("C:/Windows/Fonts/malgun.ttf", 0.0F, &fontCfg);
    fonts.AddFontFromFileTTF("C:/Windows/Fonts/msgothic.ttc", 0.0F, &fontCfg);

    bool startLight = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--light")
        {
            startLight = true;
        }
    }
    const auto schemeConfig = ImGuiEx::M3::GetDefaultSchemeConfig(!startLight);
    ImGuiEx::M3::Initialize(FindIconFontPath(argc, argv), schemeConfig);
    if (g_uiScale > 0.0F)
    {
        ImGuiEx::M3::Context::GetM3Styles().UpdateScaling(g_uiScale);
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
            {
                done = true;
            }
        }
        if (done)
        {
            break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Pin the virtual cursor AFTER the backends have updated it, so hover
        // states render without a physical mouse over the capture area.
        if (g_hoverX >= 0.0F && g_hoverY >= 0.0F)
        {
            io.MousePos = ImVec2(g_hoverX, g_hoverY);
            if (std::getenv("PREVIEW_HOVER_MARK") != nullptr)
            {
                auto *fg = ImGui::GetForegroundDrawList();
                const ImVec2 p(g_hoverX, g_hoverY);
                fg->AddLine({p.x - 12, p.y}, {p.x + 12, p.y}, IM_COL32(255, 64, 64, 255), 2.0F);
                fg->AddLine({p.x, p.y - 12}, {p.x, p.y + 12}, IM_COL32(255, 64, 64, 255), 2.0F);
                fg->AddCircle(p, 4.0F, IM_COL32(255, 64, 64, 255), 0, 2.0F);
            }
        }

        // Mirror the game's per-frame style sync (ImeWnd::Draw): theme switches
        // rebuild the scheme mid-frame, and only a frame-start refresh keeps
        // the base ImGui style from being rolled back by open style guards.
        ImGuiEx::M3::SetupDefaultImGuiStyles(ImGui::GetStyle());
        {
            // Mirror ImeWnd::Draw's frame-level LabelLarge role scope: the game
            // wraps candidate window + overlay + tool window in it, so the chip
            // lambdas' UseTextRole<LabelLarge> hits the same-role early-out.
            auto &m3Styles   = ImGuiEx::M3::Context::GetM3Styles();
            const auto scope = m3Styles.UseTextRole<ImGuiEx::M3::Spec::TextRole::LabelLarge>();
            if (std::getenv("PREVIEW_LANGBAR") != nullptr)
            {
                DrawMockLanguageBar();
            }
            else
            {
                DrawMockToolWindow();
            }
        }

        // PREVIEW_DUMP_TOOLTIP=1: after each frame, dump the internals of every
        // live tooltip window (rects, measured content, scroll, font size) to
        // preview_debug.txt — diagnosis for clipped tooltip text.
        if (std::getenv("PREVIEW_DUMP_TOOLTIP") != nullptr && ImGui::GetFrameCount() % 60 == 0)
        {
            if (FILE *f = nullptr; fopen_s(&f, "preview_debug.txt", "a") == 0 && f != nullptr)
            {
                ImGuiContext &g = *ImGui::GetCurrentContext();
                const ImRect zone(560.0F, 440.0F, 800.0F, 490.0F);
                auto dumpOne = [&](const char *label, const ImGuiWindow *w, const ImDrawList *dl) {
                    // Only commands whose bbox intersects the tooltip text zone.
                    for (int i = 0; i < dl->CmdBuffer.Size; ++i)
                    {
                        const ImDrawCmd &cmd = dl->CmdBuffer[i];
                        if (cmd.ElemCount == 0) continue;
                        float cx1 = 1e9F, cy1 = 1e9F, cx2 = -1e9F, cy2 = -1e9F;
                        for (int e = 0; e < cmd.ElemCount; ++e)
                        {
                            const ImDrawVert &v = dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset + e]];
                            cx1 = ImMin(cx1, v.pos.x); cy1 = ImMin(cy1, v.pos.y);
                            cx2 = ImMax(cx2, v.pos.x); cy2 = ImMax(cy2, v.pos.y);
                        }
                        const ImRect cmdRect(cx1, cy1, cx2, cy2);
                        if (!zone.Overlaps(cmdRect)) continue;
                        fprintf(f, "frame %d %s window '%s' flags=%08X pos=(%.0f,%.0f) size=(%.0f,%.0f)\n",
                                ImGui::GetFrameCount(), label, w != nullptr ? w->Name : "?", w != nullptr ? w->Flags : 0,
                                w != nullptr ? w->Pos.x : 0.0F, w != nullptr ? w->Pos.y : 0.0F, w != nullptr ? w->Size.x : 0.0F, w != nullptr ? w->Size.y : 0.0F);
                        fprintf(f, "  cmd[%d] elemCount=%d clip=(%.1f,%.1f)-(%.1f,%.1f) cmdBBox=(%.2f,%.2f)-(%.2f,%.2f)\n",
                                i, cmd.ElemCount, cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w, cx1, cy1, cx2, cy2);
                        // Per-quad bboxes for quads fully inside the zone.
                        if (cmd.ElemCount % 6 == 0)
                        {
                            for (int q = 0; q < cmd.ElemCount / 6; ++q)
                            {
                                float qx1 = 1e9F, qy1 = 1e9F, qx2 = -1e9F, qy2 = -1e9F;
                                for (int e = 0; e < 6; ++e)
                                {
                                    const ImDrawVert &v = dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset + q * 6 + e]];
                                    qx1 = ImMin(qx1, v.pos.x); qy1 = ImMin(qy1, v.pos.y);
                                    qx2 = ImMax(qx2, v.pos.x); qy2 = ImMax(qy2, v.pos.y);
                                }
                                if (qx1 >= 560.0F && qx2 <= 800.0F && qy1 >= 440.0F && qy2 <= 490.0F)
                                    fprintf(f, "   quad[%04d] x=(%.2f..%.2f) y=(%.2f..%.2f) w=%.2f col=%08X\n", q, qx1, qx2, qy1, qy2, qx2 - qx1, dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset + q * 6]].col);
                            }
                        }
                    }
                };
                for (const ImGuiWindow *w : g.Windows)
                {
                    if (w->DrawListInst.VtxBuffer.Size == 0) continue;
                    dumpOne("win", w, &w->DrawListInst);
                }
                if (ImGui::GetForegroundDrawList()->VtxBuffer.Size > 0)
                    dumpOne("FG", nullptr, ImGui::GetForegroundDrawList());
                fclose(f);
            }
        }

        ImGui::Render();
        const float clear[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);

        if (GetAsyncKeyState(VK_ESCAPE) & 0x1)
        {
            done = true;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGuiEx::M3::Destroy();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    UnregisterClassW(WINDOW_CLASS, wc.hInstance);
    return 0;
}
