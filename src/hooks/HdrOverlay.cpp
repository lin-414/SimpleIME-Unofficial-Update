#include "hooks/HdrOverlay.h"

#include "hooks/HdrOverlayLogic.h"
#include "imgui.h"
#include "log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_MSC_VER)
    #pragma comment(lib, "d3dcompiler") // as in imgui_impl_dx11.cpp: D3DCompile() below
#endif

namespace Hooks::HdrOverlay
{
namespace
{
using HdrOverlayLogic::CsHdrSettings;
using HdrOverlayLogic::OutputMode;

/// Community Shaders' user settings — the same file the feature itself reads
/// its "HDR Display" toggles from. Resolved through the game's working
/// directory, so MO2's VFS merge applies.
constexpr const char *kCsSettingsPath = R"(Data\SKSE\Plugins\CommunityShaders\SettingsUser.json)";
/// How often the settings file and the display state are re-checked (the user
/// can toggle HDR or retune paper white from the Community Shaders menu at any
/// time).
constexpr std::uint64_t kStateRecheckIntervalMs = 2000;
/// Decision logging is rate limited and skips repeats: the mode legitimately
/// alternates every frame between the engine's UI target and the swap chain
/// back buffer.
constexpr std::uint64_t kModeLogIntervalMs = 1000;

ID3D11Device        *g_device    = nullptr;
ID3D11DeviceContext *g_context   = nullptr;
IDXGISwapChain      *g_swapChain = nullptr; ///< only used to reach the output the display state is read from

OutputMode     g_mode{};
CsHdrSettings  g_settings{};
bool           g_monitorHdrActive = false;
std::uint64_t  g_stateNextCheckMs = 0;
std::filesystem::file_time_type g_settingsWriteTime{};
bool           g_settingsWriteTimeKnown   = false;
bool           g_settingsReadFailureLogged = false;

ID3D11PixelShader *g_pixelShader = nullptr;
float              g_pixelShaderPaperWhite = 0.0F;
float              g_pixelShaderBrightness = 0.0F;

std::uint64_t g_lastModeLogMs  = 0;
std::uint8_t  g_lastLoggedMode = 0;

/// Mirrors Community Shaders' HDROutputCS.hlsl UI path; the source itself lives
/// in HdrOverlayLogic.h so the headless tests compile it too.
auto CompilePixelShader(const float paperWhite, const float uiBrightness) -> ID3D11PixelShader *
{
    if (g_device == nullptr)
    {
        return nullptr;
    }
    const auto source = HdrOverlayLogic::BuildPixelShaderSource(paperWhite, uiBrightness);

    ID3DBlob     *blob   = nullptr;
    ID3DBlob     *errors = nullptr;
    const HRESULT compileResult =
        D3DCompile(source.c_str(), source.size(), "SimpleIME_HdrOverlay", nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &errors);
    if (FAILED(compileResult) || blob == nullptr)
    {
        logger::error(
            "[HDR] overlay pixel shader failed to compile: {}",
            errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "no compiler output"
        );
        if (errors != nullptr)
        {
            errors->Release();
        }
        if (blob != nullptr)
        {
            blob->Release();
        }
        return nullptr;
    }
    if (errors != nullptr)
    {
        errors->Release();
    }

    ID3D11PixelShader *shader       = nullptr;
    const HRESULT      createResult = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
    blob->Release();
    if (FAILED(createResult) || shader == nullptr)
    {
        logger::error("[HDR] CreatePixelShader failed: {:#x}", static_cast<std::uint32_t>(createResult));
        return nullptr;
    }
    return shader;
}

/// The shader for the current parameters, compiled on first use and recompiled
/// whenever Community Shaders' HDR values change.
auto EnsurePixelShader() -> ID3D11PixelShader *
{
    if (g_pixelShader != nullptr && g_pixelShaderPaperWhite == g_settings.paperWhite && g_pixelShaderBrightness == g_settings.uiBrightness)
    {
        return g_pixelShader;
    }
    if (g_pixelShader != nullptr)
    {
        g_pixelShader->Release();
        g_pixelShader = nullptr;
    }
    g_pixelShader = CompilePixelShader(g_settings.paperWhite, g_settings.uiBrightness);
    if (g_pixelShader == nullptr)
    {
        return nullptr;
    }
    g_pixelShaderPaperWhite = g_settings.paperWhite;
    g_pixelShaderBrightness = g_settings.uiBrightness;
    logger::info(
        "[HDR] overlay color transform active: HDR10/PQ, paperWhite={} nits, uiBrightness={}",
        g_settings.paperWhite,
        g_settings.uiBrightness
    );
    return g_pixelShader;
}

/// Draw callback queued as this frame's first command: every ImGui draw after it
/// runs with the HDR shader. The backend restores the engine's pixel shader when
/// ImGui_ImplDX11_RenderDrawData returns.
void BindPixelShader(const ImDrawList * /*a_parentList*/, const ImDrawCmd * /*a_cmd*/)
{
    if (g_context != nullptr && g_pixelShader != nullptr)
    {
        g_context->PSSetShader(g_pixelShader, nullptr, 0);
    }
}

auto QueryBoundTargetFormat(std::uint32_t &a_format) -> bool
{
    if (g_context == nullptr)
    {
        return false;
    }
    ID3D11RenderTargetView *rtv = nullptr;
    g_context->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv == nullptr)
    {
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    rtv->GetDesc(&desc);
    a_format = static_cast<std::uint32_t>(desc.Format);
    rtv->Release();
    return true;
}

/// Whether the display the swap chain sits on runs in HDR10 (PQ/BT.2020) mode —
/// the state Community Shaders' startup auto-detect reads. Same check as its
/// DetectHDR() DXGI fallback: the containing output's current color space.
/// Returns false when the output cannot be reached (Streamline/frame generation
/// can hide it), which leaves the caller's last known value in place.
auto QueryMonitorHdrActive(bool &a_hdrActive) -> bool
{
    if (g_swapChain == nullptr)
    {
        return false;
    }
    IDXGIOutput *output = nullptr;
    if (FAILED(g_swapChain->GetContainingOutput(&output)) || output == nullptr)
    {
        return false;
    }
    bool          known   = false;
    IDXGIOutput6 *output6 = nullptr;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && output6 != nullptr)
    {
        DXGI_OUTPUT_DESC1 desc{};
        if (SUCCEEDED(output6->GetDesc1(&desc)))
        {
            a_hdrActive = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            known       = true;
        }
        output6->Release();
    }
    output->Release();
    return known;
}

/// Re-read Community Shaders' settings when the file changed, and the display
/// state, at most once per kStateRecheckIntervalMs.
void RefreshStateIfDue()
{
    const auto now = GetTickCount64();
    if (now < g_stateNextCheckMs)
    {
        return;
    }
    g_stateNextCheckMs = now + kStateRecheckIntervalMs;
    bool monitorHdr    = false;
    if (QueryMonitorHdrActive(monitorHdr))
    {
        g_monitorHdrActive = monitorHdr;
    }

    const std::filesystem::path path{ kCsSettingsPath };
    std::error_code             error;
    const auto                  writeTime    = std::filesystem::last_write_time(path, error);
    const bool                  hasWriteTime = !error;
    if (hasWriteTime && g_settingsWriteTimeKnown && writeTime == g_settingsWriteTime)
    {
        return;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        // No Community Shaders (or no user settings yet): nothing to read, the
        // display state stays the only signal.
        if (!g_settingsReadFailureLogged)
        {
            g_settingsReadFailureLogged = true;
            logger::debug("[HDR] no Community Shaders settings at {}; HDR state comes from the display", kCsSettingsPath);
        }
        return;
    }
    const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    if (!HdrOverlayLogic::LooksLikeCompleteJsonObject(text))
    {
        // Caught the file while Community Shaders was rewriting it; the next
        // check reads it again instead of latching a half-written state.
        return;
    }
    const auto settings = HdrOverlayLogic::ParseCommunityShadersHdrSettings(text);
    g_settingsWriteTime      = writeTime;
    g_settingsWriteTimeKnown = hasWriteTime;
    if (settings == g_settings)
    {
        return;
    }
    g_settings = settings;
    logger::info(
        "[HDR] Community Shaders settings: HDR {} (auto-detected {}, paperWhite={} nits, uiBrightness={})",
        g_settings.enableHdr ? "enabled" : "disabled",
        g_settings.autoDetected ? "yes" : "no",
        g_settings.paperWhite,
        g_settings.uiBrightness
    );
}

void LogDecision(const std::uint32_t format, const OutputMode mode)
{
    // The mode flips every frame between the engine's UI target (None) and the
    // swap chain back buffer, so the same decision must not re-log itself.
    const auto modeValue = static_cast<std::uint8_t>(mode);
    if (modeValue == g_lastLoggedMode)
    {
        return;
    }
    const auto now = GetTickCount64();
    if (now - g_lastModeLogMs < kModeLogIntervalMs)
    {
        return;
    }
    g_lastModeLogMs  = now;
    g_lastLoggedMode = modeValue;
    logger::debug(
        "[HDR] overlay target format {} -> mode {} (CS HDR {}, auto-detect {}, display HDR {})",
        format,
        static_cast<int>(mode),
        g_settings.enableHdr ? "on" : "off",
        g_settings.autoDetected ? "done" : "pending",
        g_monitorHdrActive ? "on" : "off"
    );
}
} // namespace

void Initialize(ID3D11Device *a_device, ID3D11DeviceContext *a_context)
{
    Shutdown();
    g_device  = a_device;
    g_context = a_context;
    // Read the settings on the first frame rather than at init: the file lives
    // in the game's Data directory, which the engine has fully resolved by then.
    g_stateNextCheckMs = 0;
}

void SetSwapChain(void *a_swapChain)
{
    if (g_swapChain != nullptr)
    {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (a_swapChain == nullptr)
    {
        return;
    }
    // Only the v1 interface is needed (GetContainingOutput). Community Shaders'
    // frame-generation swap chain proxy implements exactly that one, and the
    // detection does not depend on anything beyond it.
    auto *swapChain = reinterpret_cast<IDXGISwapChain *>(a_swapChain);
    swapChain->AddRef();
    g_swapChain = swapChain;
}

void PrepareFrame()
{
    if (g_device == nullptr || g_context == nullptr)
    {
        return;
    }
    RefreshStateIfDue();

    std::uint32_t format = 0;
    if (!QueryBoundTargetFormat(format))
    {
        return;
    }
    const auto mode = HdrOverlayLogic::DecideOutputMode(format, g_settings, g_monitorHdrActive);
    if (mode != g_mode)
    {
        g_mode = mode;
        if (mode != OutputMode::None)
        {
            LogDecision(format, mode);
        }
    }
    if (mode == OutputMode::None || EnsurePixelShader() == nullptr)
    {
        return;
    }
    ImGui::GetBackgroundDrawList()->AddCallback(&BindPixelShader, nullptr);
}

void Shutdown()
{
    if (g_pixelShader != nullptr)
    {
        g_pixelShader->Release();
        g_pixelShader = nullptr;
    }
    if (g_swapChain != nullptr)
    {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    g_pixelShaderPaperWhite  = 0.0F;
    g_pixelShaderBrightness  = 0.0F;
    g_device                 = nullptr;
    g_context                = nullptr;
    g_mode                   = OutputMode::None;
    g_settings               = CsHdrSettings{};
    g_monitorHdrActive       = false;
    g_stateNextCheckMs       = 0;
    g_settingsWriteTimeKnown = false;
    g_settingsReadFailureLogged = false;
    g_lastLoggedMode         = 0;
}
} // namespace Hooks::HdrOverlay
