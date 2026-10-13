//
// Headless tests for the HDR overlay's pure decision logic (extracted into
// HdrOverlayLogic.h so no D3D/Windows machinery is needed here): which output
// mode a render target needs, and the Community Shaders settings read behind it.
//
#include "hooks/HdrOverlayLogic.h"

#include <d3dcompiler.h>

#include <gtest/gtest.h>

#if defined(_MSC_VER)
    #pragma comment(lib, "d3dcompiler") // the plugin links it the same way (imgui_impl_dx11.cpp)
#endif

using namespace Hooks::HdrOverlayLogic;

namespace
{
constexpr auto kFormatR8G8B8A8Unorm = 28U;

/// A verbatim slice of Community Shaders' SettingsUser.json.
constexpr auto kSettingsJson = R"({
 "Advanced": {
  "Compiler Threads": 30
 },
 "HDR Display": {
  "dontShowHDRWarning": false,
  "enableHDR": true,
  "hdrAutoDetected": true,
  "hdrPaperWhite": 203,
  "hdrPeakNits": 800,
  "hdrUIBrightness": 1.0
 },
 "Hair Backlighting": {
  "EnableHairLighting": true
 }
}
)";

constexpr auto kHdrEnabled = CsHdrSettings{ .fileRead = true, .enableHdr = true, .autoDetected = true, .paperWhite = 203.0F, .uiBrightness = 1.0F };
} // namespace

TEST(HdrOverlayDecideOutputMode, leaves_the_engine_ui_target_alone)
{
    // Community Shaders decodes its own UI buffer as sRGB and composites it
    // itself — HDR or not, an 8-bit target needs no transform.
    EXPECT_EQ(DecideOutputMode(kFormatR8G8B8A8Unorm, kHdrEnabled, true), OutputMode::None);
    EXPECT_EQ(DecideOutputMode(kFormatR8G8B8A8Unorm, CsHdrSettings{}, true), OutputMode::None);
    EXPECT_EQ(DecideOutputMode(kFormatR8G8B8A8Unorm, CsHdrSettings{}, false), OutputMode::None);
}

TEST(HdrOverlayDecideOutputMode, encodes_for_the_hdr_swap_chain_formats)
{
    EXPECT_EQ(DecideOutputMode(kFormatR10G10B10A2Unorm, kHdrEnabled, true), OutputMode::Hdr10Pq);
    EXPECT_EQ(DecideOutputMode(kFormatR16G16B16A16Float, kHdrEnabled, true), OutputMode::Hdr10Pq);
}

TEST(HdrOverlayDecideOutputMode, respects_hdr_being_switched_off)
{
    // The feature is loaded (10-bit back buffer) but HDR is off: the buffer
    // holds plain sRGB, and the display's own HDR state must not override the
    // user's choice.
    const CsHdrSettings disabled{ .fileRead = true, .enableHdr = false, .autoDetected = true, .paperWhite = 203.0F, .uiBrightness = 1.0F };
    EXPECT_EQ(DecideOutputMode(kFormatR10G10B10A2Unorm, disabled, true), OutputMode::None);
    EXPECT_EQ(DecideOutputMode(kFormatR10G10B10A2Unorm, disabled, false), OutputMode::None);
}

TEST(HdrOverlayDecideOutputMode, defers_to_the_display_before_auto_detect)
{
    // Community Shaders enables HDR for an HDR display at startup and persists
    // that only later, so an undecided auto-detect means the display decides.
    EXPECT_EQ(DecideOutputMode(kFormatR10G10B10A2Unorm, CsHdrSettings{}, true), OutputMode::Hdr10Pq);
    EXPECT_EQ(DecideOutputMode(kFormatR10G10B10A2Unorm, CsHdrSettings{}, false), OutputMode::None);

    const CsHdrSettings undecided{ .fileRead = true, .enableHdr = false, .autoDetected = false, .paperWhite = 203.0F, .uiBrightness = 1.0F };
    EXPECT_EQ(DecideOutputMode(kFormatR10G10B10A2Unorm, undecided, true), OutputMode::Hdr10Pq);
    EXPECT_EQ(DecideOutputMode(kFormatR10G10B10A2Unorm, undecided, false), OutputMode::None);
}

TEST(HdrOverlayPixelShader, compiles_with_the_same_compiler_the_plugin_uses)
{
    // A shader that fails to compile in game would silently leave the overlay
    // blown out, so the syntax is checked here — with D3DCompile, exactly as
    // HdrOverlay.cpp does at runtime.
    for (const float paperWhite : { 203.0F, 300.0F })
    {
        const auto source = BuildPixelShaderSource(paperWhite, 1.0F);
        ID3DBlob  *blob   = nullptr;
        ID3DBlob  *errors = nullptr;
        const HRESULT result = D3DCompile(source.c_str(), source.size(), "HdrOverlayTest", nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &errors);
        EXPECT_TRUE(SUCCEEDED(result)) << (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "no compiler output");
        if (errors != nullptr)
        {
            errors->Release();
        }
        if (blob != nullptr)
        {
            EXPECT_GT(blob->GetBufferSize(), 0U);
            blob->Release();
        }
    }
}

TEST(HdrOverlaySettingsParse, reads_the_hdr_display_section)
{
    const auto settings = ParseCommunityShadersHdrSettings(kSettingsJson);
    EXPECT_TRUE(settings.fileRead);
    EXPECT_TRUE(settings.enableHdr);
    EXPECT_TRUE(settings.autoDetected);
    EXPECT_FLOAT_EQ(settings.paperWhite, 203.0F);
    EXPECT_FLOAT_EQ(settings.uiBrightness, 1.0F);
}

TEST(HdrOverlaySettingsParse, keeps_defaults_without_the_section)
{
    const auto settings = ParseCommunityShadersHdrSettings(R"({ "Advanced": { "Log Level": 2 } })");
    EXPECT_FALSE(settings.fileRead);
    EXPECT_FALSE(settings.enableHdr);
    EXPECT_FALSE(settings.autoDetected);
    EXPECT_FLOAT_EQ(settings.paperWhite, kDefaultPaperWhite);
    EXPECT_FLOAT_EQ(settings.uiBrightness, kDefaultUiBrightness);
}

TEST(HdrOverlaySettingsParse, survives_malformed_and_partial_input)
{
    EXPECT_FALSE(ParseCommunityShadersHdrSettings("").fileRead);
    EXPECT_FALSE(ParseCommunityShadersHdrSettings("{").fileRead);
    EXPECT_FALSE(ParseCommunityShadersHdrSettings(R"({"HDR Display": {)").fileRead);

    // Section present, members missing or of the wrong shape: the defaults hold
    // and only the readable members are taken.
    const auto partial = ParseCommunityShadersHdrSettings(R"({"HDR Display": {"enableHDR": true, "hdrPaperWhite": "bright"}})");
    EXPECT_TRUE(partial.fileRead);
    EXPECT_TRUE(partial.enableHdr);
    EXPECT_FALSE(partial.autoDetected);
    EXPECT_FLOAT_EQ(partial.paperWhite, kDefaultPaperWhite);
    EXPECT_FLOAT_EQ(partial.uiBrightness, kDefaultUiBrightness);

    // A key that only looks like the section name must not match.
    EXPECT_FALSE(ParseCommunityShadersHdrSettings(R"({"HDR Display Advanced": {"enableHDR": true}})").fileRead);
}

TEST(HdrOverlaySettingsParse, clamps_implausible_values)
{
    EXPECT_FLOAT_EQ(ParseCommunityShadersHdrSettings(R"({"HDR Display": {"hdrPaperWhite": 0}})").paperWhite, kDefaultPaperWhite);
    EXPECT_FLOAT_EQ(ParseCommunityShadersHdrSettings(R"({"HDR Display": {"hdrPaperWhite": 99999}})").paperWhite, kDefaultPaperWhite);
    EXPECT_FLOAT_EQ(ParseCommunityShadersHdrSettings(R"({"HDR Display": {"hdrPaperWhite": 500}})").paperWhite, 500.0F);
    EXPECT_FLOAT_EQ(ParseCommunityShadersHdrSettings(R"({"HDR Display": {"hdrUIBrightness": 0}})").uiBrightness, kDefaultUiBrightness);
    EXPECT_FLOAT_EQ(ParseCommunityShadersHdrSettings(R"({"HDR Display": {"hdrUIBrightness": 2.5}})").uiBrightness, 2.5F);
}

TEST(HdrOverlaySettingsParse, rejects_half_written_files)
{
    EXPECT_TRUE(LooksLikeCompleteJsonObject("{}"));
    EXPECT_TRUE(LooksLikeCompleteJsonObject("  {\n \"HDR Display\": {}\n} \n"));
    EXPECT_FALSE(LooksLikeCompleteJsonObject(""));
    EXPECT_FALSE(LooksLikeCompleteJsonObject("   \n"));
    EXPECT_FALSE(LooksLikeCompleteJsonObject("{"));                            // truncated mid-write
    EXPECT_FALSE(LooksLikeCompleteJsonObject(R"({"HDR Display": {"enableHDR": tr)"));
    EXPECT_FALSE(LooksLikeCompleteJsonObject("[1, 2]"));                       // not an object
}

TEST(HdrOverlaySettingsParse, reads_disabled_and_whitespace_variants)
{
    const auto disabled = ParseCommunityShadersHdrSettings(
        "{\"HDR Display\":{\"enableHDR\":false,\"hdrAutoDetected\":true,\"hdrPaperWhite\":300,\"hdrUIBrightness\":1.5}}"
    );
    EXPECT_TRUE(disabled.fileRead);
    EXPECT_FALSE(disabled.enableHdr);
    EXPECT_TRUE(disabled.autoDetected);
    EXPECT_FLOAT_EQ(disabled.paperWhite, 300.0F);
    EXPECT_FLOAT_EQ(disabled.uiBrightness, 1.5F);
}
