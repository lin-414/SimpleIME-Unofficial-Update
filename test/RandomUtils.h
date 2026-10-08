//
// Created by jamie on 2026/1/17.
//
#pragma once

#include "configs/configuration.h"

#include <random>

namespace ImeTest
{

class RandomUtils
{
    std::mt19937  gen;
    std::uint32_t seed;

public:
    /// Seed is injectable so a failing random round-trip can be reproduced:
    /// pass the recorded seed back in instead of rerolling.
    explicit RandomUtils(std::uint32_t seedValue = std::random_device{}()) : gen(seedValue), seed(seedValue) {}

    [[nodiscard]] auto Seed() const -> std::uint32_t { return seed; }

    auto NextInt(int min, int max) -> int { return std::uniform_int_distribution<>(min, max)(gen); }

    auto NextBool() -> bool { return std::bernoulli_distribution(0.5)(gen); }

    auto NextFloat(float min, float max) -> float { return std::uniform_real_distribution(static_cast<double>(min), static_cast<double>(max))(gen); }

    auto NextString(size_t length) -> std::string
    {
        std::uniform_int_distribution<> dis('a', 'z');

        std::string s;
        for (size_t i = 0; i < length; ++i)
        {
            s += static_cast<char>(dis(gen));
        }
        return s;
    }
};

inline auto GetRandomConfiguation(std::uint32_t *seedOut = nullptr) -> Ime::Configuration
{
    const auto seed = std::random_device{}();
    if (seedOut != nullptr)
    {
        *seedOut = seed;
    }
    ImeTest::RandomUtils random{seed};
    Ime::Configuration   configuration{};
    configuration.shortcut                      = random.NextString(10);
    configuration.enableMod                     = random.NextBool();
    configuration.enableTsf                     = random.NextBool();
    configuration.fixInconsistentTextEntryCount = random.NextBool();
    configuration.autoToggleKeyboard            = random.NextBool();
    configuration.switchEnglishLayoutOnDisable  = random.NextBool();
    configuration.forceDpiAwareness             = random.NextBool();

    configuration.logging.level      = random.NextString(10);
    configuration.logging.flushLevel = random.NextString(10);

    configuration.resources.translationDir = random.NextString(10);
    configuration.resources.fontPathList   = std::vector{random.NextString(10), random.NextString(10)};

    configuration.appearance.zoom                  = std::round(random.NextFloat(1.f, 9999.f) * 100.f) / 100.f;
    configuration.appearance.themeStyle            = random.NextBool() ? "default" : "material";
    configuration.appearance.themeSourceColor      = random.NextInt(0, 0xffffff);
    configuration.appearance.themeDarkMode         = random.NextBool();
    configuration.appearance.themeContrastLevel    = 0.5;
    configuration.appearance.language              = random.NextString(10);
    configuration.appearance.errorDisplayDuration  = random.NextInt(0, 0xffff);
    configuration.appearance.verticalCandidateList = random.NextBool();
    configuration.appearance.autoToggleLanguageBar = random.NextBool();

    configuration.input.enableUnicodePaste       = random.NextBool();
    configuration.input.keepImeOpen              = random.NextBool();
    configuration.input.posUpdatePolicy          = random.NextString(10);
    configuration.input.meridianSupport          = random.NextBool();
    configuration.input.prismaAvoidance          = random.NextBool();
    configuration.input.skseMenuFrameworkSupport = random.NextBool();
    configuration.input.lastNativeConversion     = random.NextBool();
    return configuration;
}

} // namespace ImeTest
