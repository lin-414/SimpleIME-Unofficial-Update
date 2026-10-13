#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;

// Overlay color pipeline for HDR swap chains (Community Shaders' "HDR Display"
// feature). SimpleIME draws the candidate window into the swap chain back
// buffer at present time so it stays above host menus (SKSEMF, Prisma, FUCK);
// under HDR that buffer holds PQ-encoded HDR10 values, and plain sRGB colors
// written into it come out blown out. This routes the frame's ImGui draws
// through a pixel shader that encodes them the way Community Shaders encodes
// the engine's own UI, and leaves everything else (8-bit back buffers, the
// engine's UI target) on the stock shader.
//
// The mode decision itself is pure logic in HdrOverlayLogic.h.
namespace Hooks::HdrOverlay
{
/// Cache the game's device/context and drop state from a previous device.
/// Called once per D3D init (device resets re-enter it).
void Initialize(ID3D11Device *a_device, ID3D11DeviceContext *a_context);

/// Remember the swap chain whose color space reports whether HDR output is
/// live. Takes the raw pointer the D3D init hook already has; re-callable.
void SetSwapChain(void *a_swapChain);

/// Per-frame work, called right after ImGui::NewFrame(): looks at the render
/// target bound right now and, when the overlay would land in an HDR surface,
/// queues the pixel shader for this frame's draws.
void PrepareFrame();

/// Release everything (device reset / plugin shutdown). PrepareFrame stays a
/// no-op until the next Initialize.
void Shutdown();
} // namespace Hooks::HdrOverlay
