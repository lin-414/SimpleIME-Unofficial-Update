#pragma once

#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

// Pure, D3D-free decision logic of the HDR overlay color pipeline, extracted so
// it can be unit-tested headless (HdrOverlay.cpp keeps the device/shader
// plumbing). Same split SkseMenuFrameworkBridgeLogic.h uses.
//
// Why this exists: the candidate window (and every other SimpleIME surface that
// has to out-draw a host menu) is rendered straight into the swap chain back
// buffer at present time. When Community Shaders' "HDR Display" feature is
// active that back buffer is a 10-bit HDR10 buffer holding BT.2020/ST.2084 (PQ)
// encoded values — an overlay writing plain sRGB colors into it is read as PQ
// and comes out blown out (white surfaces clip, colors wash out). The transform
// reproduces what Community Shaders itself does with the game's UI texture
// (sRGB->linear -> BT.709 to BT.2020 -> PQ at paper white), so the overlay
// matches the engine UI drawn next to it.
namespace Hooks::HdrOverlayLogic
{
/// How the overlay's sRGB colors must be encoded for the surface they are drawn
/// into.
enum class OutputMode : std::uint8_t
{
    /// Plain sRGB passthrough: an 8-bit back buffer, or one of the engine's own
    /// UI render targets (Community Shaders composites those itself, decoding
    /// them as sRGB).
    None,
    /// HDR10: BT.2020 + ST.2084 (PQ), SDR white at the configured paper white.
    Hdr10Pq
};

// DXGI_FORMAT values, spelled out so this header stays free of the DXGI headers
// for the headless tests. A 10-bit or float back buffer is what the HDR swap
// chain upgrade produces; an 8-bit one is the plain SDR (or the engine's UI
// target) and needs nothing.
inline constexpr std::uint32_t kFormatR16G16B16A16Float = 10;
inline constexpr std::uint32_t kFormatR10G10B10A2Unorm  = 24;

/// The nits SDR white is mapped to when nothing better is known: the BT.2408
/// reference and Community Shaders' own default.
inline constexpr float kDefaultPaperWhite   = 203.0F;
inline constexpr float kDefaultUiBrightness = 1.0F;

/// Community Shaders' own HDR settings, as read from its SettingsUser.json.
/// `fileRead` false means the file was missing/unreadable or carried no
/// "HDR Display" section.
struct CsHdrSettings
{
    bool  fileRead      = false;
    bool  enableHdr     = false;
    /// Community Shaders writes this back only after its startup auto-detect
    /// ran; while it is false the display state is the better truth (see
    /// DecideOutputMode).
    bool  autoDetected  = false;
    float paperWhite    = kDefaultPaperWhite;
    float uiBrightness  = kDefaultUiBrightness;

    auto operator==(const CsHdrSettings &) const -> bool = default;
};

/// The output mode for the render target the overlay is about to draw into.
///
/// The target's format is the first gate: Community Shaders' swap chain upgrade
/// is the only thing that turns the back buffer into a 10-bit/float surface, and
/// the engine's own UI buffer (which the feature decodes as sRGB and composites
/// itself) must stay untouched.
///
/// The rest follows Community Shaders' own state, because DXGI cannot report a
/// swap chain's color space back (only SetColorSpace1 exists): `cs` is its
/// settings file, `monitorHdrActive` the HDR10 mode of the output the swap chain
/// sits on — the state its auto-detect reads. The feature enables HDR for such a
/// display at startup and persists that only later, so an undecided auto-detect
/// defers to the display.
inline auto DecideOutputMode(const std::uint32_t boundTargetFormat, const CsHdrSettings &cs, const bool monitorHdrActive) -> OutputMode
{
    const bool hdrCapableTarget = boundTargetFormat == kFormatR10G10B10A2Unorm || boundTargetFormat == kFormatR16G16B16A16Float;
    if (!hdrCapableTarget)
    {
        return OutputMode::None;
    }
    if (cs.fileRead && cs.enableHdr)
    {
        return OutputMode::Hdr10Pq;
    }
    if (!cs.autoDetected && monitorHdrActive)
    {
        return OutputMode::Hdr10Pq;
    }
    return OutputMode::None;
}

/// The overlay pixel shader, mirroring Community Shaders' HDROutputCS.hlsl UI
/// path: its texture sample and vertex color multiply, then SrgbToLinear ->
/// BT.709 to BT.2020 -> ST.2084 PQ at the configured paper white (both the
/// gamma 2.2 approximation and the matrix are copied from its
/// Shaders/Common/Color.hlsli). Alpha stays untouched — the ImGui backend
/// blends the result straight into the PQ-encoded back buffer.
/// The prelude bakes the parameters in, so no constant buffer or extra render
/// state has to be tracked next to the ImGui backend's own. Lives here (rather
/// than in HdrOverlay.cpp) so the headless tests can compile it with the same
/// D3DCompile the plugin uses.
inline constexpr std::string_view kPixelShaderBody = R"(
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
sampler sampler0;
Texture2D texture0;

static const float3x3 BT709_TO_BT2020 = float3x3(
    0.627403914928436279296875,      0.3292830288410186767578125,  0.0433130674064159393310546875,
    0.069097287952899932861328125,   0.9195404052734375,           0.011362315155565738677978515625,
    0.01639143936336040496826171875, 0.08801330626010894775390625, 0.895595252513885498046875);

// ST.2084 (PQ) encode of a linear BT.2020 value; `paperWhite` nits is SDR white.
float3 PqEncode(float3 color, float paperWhite)
{
    color *= paperWhite / 10000.0;
    float3 y = pow(max(0.0, color), 2610.0 / 16384.0);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

float4 main(PS_INPUT input) : SV_Target
{
    float4 color = input.col * texture0.Sample(sampler0, input.uv);
    // (`linear` is an HLSL keyword, hence the name.)
    float3 uiLinear = pow(abs(color.rgb), 2.2) * UI_BRIGHTNESS;
    color.rgb = PqEncode(mul(BT709_TO_BT2020, uiLinear), PAPER_WHITE);
    return color;
}
)";

/// The shader source for one paper white / UI brightness pair.
inline auto BuildPixelShaderSource(const float paperWhite, const float uiBrightness) -> std::string
{
    return std::format("#define PAPER_WHITE {:.6f}\n#define UI_BRIGHTNESS {:.6f}\n{}", paperWhite, uiBrightness, kPixelShaderBody);
}

namespace detail
{
/// The object that follows the key text `key` (quotes included, e.g. `"HDR
/// Display"`) in a JSON document, without its braces. Community Shaders'
/// settings file is a flat map of section -> scalar object, so a full JSON
/// parser would be all dependency and no value here.
inline auto FindJsonObject(const std::string_view json, const std::string_view key) -> std::optional<std::string_view>
{
    std::size_t search = 0;
    while (true)
    {
        const auto keyPos = json.find(key, search);
        if (keyPos == std::string_view::npos)
        {
            return std::nullopt;
        }
        search     = keyPos + key.size();
        auto cursor = keyPos + key.size();
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n'))
        {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] != ':')
        {
            continue; // a string value that merely contains the key text
        }
        ++cursor;
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n'))
        {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] != '{')
        {
            return std::nullopt;
        }
        const auto  bodyStart = cursor + 1;
        std::size_t depth     = 1;
        bool        inString  = false;
        for (cursor = bodyStart; cursor < json.size(); ++cursor)
        {
            const char ch = json[cursor];
            if (inString)
            {
                if (ch == '\\')
                {
                    ++cursor;
                }
                else if (ch == '"')
                {
                    inString = false;
                }
                continue;
            }
            if (ch == '"')
            {
                inString = true;
            }
            else if (ch == '{')
            {
                ++depth;
            }
            else if (ch == '}')
            {
                if (--depth == 0)
                {
                    return json.substr(bodyStart, cursor - bodyStart);
                }
            }
        }
        return std::nullopt;
    }
}

/// The raw token that follows the key text `key` inside `object` (up to the next
/// member or the object's end).
inline auto FindJsonValue(const std::string_view object, const std::string_view key) -> std::optional<std::string_view>
{
    const auto keyPos = object.find(key);
    if (keyPos == std::string_view::npos)
    {
        return std::nullopt;
    }
    auto cursor = keyPos + key.size();
    while (cursor < object.size() && (object[cursor] == ' ' || object[cursor] == '\t' || object[cursor] == '\r' || object[cursor] == '\n'))
    {
        ++cursor;
    }
    if (cursor >= object.size() || object[cursor] != ':')
    {
        return std::nullopt;
    }
    ++cursor;
    while (cursor < object.size() && (object[cursor] == ' ' || object[cursor] == '\t' || object[cursor] == '\r' || object[cursor] == '\n'))
    {
        ++cursor;
    }
    const auto valueStart = cursor;
    while (cursor < object.size() && object[cursor] != ',' && object[cursor] != '}')
    {
        ++cursor;
    }
    auto value = object.substr(valueStart, cursor - valueStart);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
    {
        value.remove_suffix(1);
    }
    return value.empty() ? std::nullopt : std::optional<std::string_view>{ value };
}

inline auto ParseJsonBool(const std::string_view token) -> std::optional<bool>
{
    if (token == "true")
    {
        return true;
    }
    if (token == "false")
    {
        return false;
    }
    return std::nullopt;
}

inline auto ParseJsonNumber(const std::string_view token) -> std::optional<double>
{
    // from_chars, not strtod: the game process locale must not decide whether
    // '.' is a decimal point.
    double      value  = 0.0;
    const auto *begin  = token.data();
    const auto *end    = token.data() + token.size();
    const auto  result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return std::nullopt;
    }
    return value;
}
} // namespace detail

/// Clamp a parsed paper white to a plausible HDR range; anything outside is a
/// parse artifact, not a setting.
inline auto SanitizePaperWhite(const double value) -> float
{
    if (!(value >= 50.0 && value <= 4000.0))
    {
        return kDefaultPaperWhite;
    }
    return static_cast<float>(value);
}

inline auto SanitizeUiBrightness(const double value) -> float
{
    if (!(value >= 0.1 && value <= 10.0))
    {
        return kDefaultUiBrightness;
    }
    return static_cast<float>(value);
}

/// Whether `text` is a complete JSON object. Community Shaders rewrites its
/// settings file in place, so a read can catch it half-way through — a
/// truncated file must not be latched as "no HDR section".
inline auto LooksLikeCompleteJsonObject(const std::string_view text) -> bool
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
    {
        return false;
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return end > begin && text[begin] == '{' && text[end] == '}';
}

/// Extract the "HDR Display" section of a Community Shaders SettingsUser.json.
/// Best effort by design: every failure leaves the defaults (and `fileRead`
/// false) so the caller degrades to the display's own state.
inline auto ParseCommunityShadersHdrSettings(const std::string_view json) -> CsHdrSettings
{
    CsHdrSettings settings{};
    const auto    section = detail::FindJsonObject(json, R"("HDR Display")");
    if (!section)
    {
        return settings;
    }
    settings.fileRead = true;
    if (const auto token = detail::FindJsonValue(*section, R"("enableHDR")"); token)
    {
        settings.enableHdr = detail::ParseJsonBool(*token).value_or(false);
    }
    if (const auto token = detail::FindJsonValue(*section, R"("hdrAutoDetected")"); token)
    {
        settings.autoDetected = detail::ParseJsonBool(*token).value_or(false);
    }
    if (const auto token = detail::FindJsonValue(*section, R"("hdrPaperWhite")"); token)
    {
        settings.paperWhite = SanitizePaperWhite(detail::ParseJsonNumber(*token).value_or(static_cast<double>(kDefaultPaperWhite)));
    }
    if (const auto token = detail::FindJsonValue(*section, R"("hdrUIBrightness")"); token)
    {
        settings.uiBrightness = SanitizeUiBrightness(detail::ParseJsonNumber(*token).value_or(static_cast<double>(kDefaultUiBrightness)));
    }
    return settings;
}
} // namespace Hooks::HdrOverlayLogic
