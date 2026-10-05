//
// Created by jamie on 25-1-22.
//
#include "common.h"
#include "configs/ConfigSerializer.h"
#include "ImeApp.h"
#include "log.h"

#include <spdlog/sinks/basic_file_sink.h>

// Declared before first use in PluginLoad; g_hModule is assigned in DllMain,
// g_skseVersion in PluginLoad — both read by other TUs (ImeWnd, ToolWindow).
namespace Ime::Global
{
auto g_hModule     = HMODULE();
auto g_skseVersion = std::string{}; ///< "2.2.6"-style, set in PluginLoad; read by the diagnostics copy
}

namespace SksePlugin
{
void InitializeLogging(SpdLogSettings settings)
{
    auto path = SKSE::log::log_directory();
    if (!path)
    {
        SKSE::stl::report_and_fail("Unable to lookup SKSE logs directory.");
    }
    *path /= SKSE::PluginDeclaration::GetSingleton()->GetName();
    *path += L".log";

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log  = std::make_shared<spdlog::logger>(std::string("global log"), std::move(sink));
    log->set_level(settings.level);
    log->flush_on(settings.flushLevel);

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%-8l] [%t] [%s:%#] %v");
}

//! SkyrimSE.exe declares no DPI awareness (no manifest entry, no DPI API in its
//! import table), so on a scaled desktop Windows bitmap-stretches the whole
//! frame by the scale factor — every pixel SimpleIME renders into the swapchain
//! (settings window, candidate list, language bar) goes soft, and the M3 scale
//! reads a virtualized 96-DPI monitor. Declaring per-monitor-v2 awareness must
//! happen BEFORE the game creates its window in WinMain, and SKSEPluginLoad is
//! the last guaranteed-before-WinMain hook SimpleIME owns. It visibly shrinks a
//! fixed-resolution window on scaled desktops (pixels stop being stretched),
//! so the shipped default is configurable via core.force_dpi_awareness.
void TryEnableProcessDpiAwareness(const bool enabled)
{
    if (!enabled)
    {
        logger::info("Process DPI awareness disabled by configuration (core.force_dpi_awareness = false).");
        return;
    }

    using PFN_SetProcessDpiAwarenessContext = BOOL (WINAPI *)(DPI_AWARENESS_CONTEXT);
    const auto setContext = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setContext != nullptr && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE)
    {
        logger::info("Process DPI awareness: per-monitor v2.");
        return;
    }

    // Pre-1703 Windows 10: shcore's SetProcessDpiAwareness (value from
    // PROCESS_DPI_AWARENESS; kept literal to avoid pulling shellscalingapi.h).
    using PFN_SetProcessDpiAwareness = HRESULT (WINAPI *)(int);
    if (const HMODULE shcore = GetModuleHandleW(L"shcore.dll"); shcore != nullptr)
    {
        const auto setAwareness = reinterpret_cast<PFN_SetProcessDpiAwareness>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setAwareness != nullptr && SUCCEEDED(setAwareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */)))
        {
            logger::info("Process DPI awareness: per-monitor (shcore fallback).");
            return;
        }
    }

    // Last resort (Vista+): system-aware — still sharp on a fixed-scale desktop.
    if (SetProcessDPIAware() != FALSE)
    {
        logger::info("Process DPI awareness: system (legacy fallback).");
        return;
    }
    // Failing everywhere almost always means another mod already set awareness
    // earlier in the load order — the desirable outcome, just not ours.
    logger::info("Process DPI awareness not changed (already set or unavailable).");
}

auto PluginLoad(const SKSE::LoadInterface *skse) -> bool
{
    try
    {
        Init(skse, false);
        if (skse != nullptr)
        {
            // Packed SKSE_VERSION_INTEGER (major<<24 | minor<<16 | revision<<8):
            // decoded once here for the settings window's diagnostics copy —
            // the load interface is not reachable later.
            const std::uint32_t packed = skse->SKSEVersion();
            Ime::Global::g_skseVersion = std::format(
                "{}.{}.{}",
                (packed >> 24) & 0xFFU,
                (packed >> 16) & 0xFFU,
                (packed >> 8) & 0xFFU);
        }
        Initialize();
        TryEnableProcessDpiAwareness(Ime::ImeApp::GetInstance().GetSettings().forceDpiAwareness);
        return true;
    }
    catch (std::exception &exception)
    {
        logger::error("Fatal error, SimpleIME init fail: {}", exception.what());
        logger::LogStacktrace();
    }
    catch (...)
    {
        logger::error("Fatal error. occur unknown exception.");
        logger::LogStacktrace();
    }
    return false;
}

auto ErrorHandler(unsigned int code, _EXCEPTION_POINTERS *) -> int
{
    logger::critical("System exception (code {}) raised during plugin initialization.", code);
    logger::LogStacktrace();
    // EXCEPTION_CONTINUE_SEARCH means the __except block in SKSEPluginLoad is
    // never entered and the process still crashes — this is a logging hook, not
    // a recovery path. Swallowing the exception would leave the game running
    // with a half-initialized plugin, which is worse than a clean crash.
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace SksePlugin

SKSEPluginLoad(const SKSE::LoadInterface *skse)
{
    __try
    {
        return SksePlugin::PluginLoad(skse);
    }
    __except (SksePlugin::ErrorHandler(GetExceptionCode(), GetExceptionInformation()))
    {
    }
    return false;
}

extern "C" auto APIENTRY DllMain(const HMODULE hModule, const DWORD ul_reason, LPVOID /*unused*/) -> BOOL
{
    switch (ul_reason)
    {
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
        case DLL_PROCESS_ATTACH:
            Ime::Global::g_hModule = hModule;
            break;
        case DLL_PROCESS_DETACH:
            spdlog::shutdown();
            break;
        default:;
    }
    return TRUE;
}
