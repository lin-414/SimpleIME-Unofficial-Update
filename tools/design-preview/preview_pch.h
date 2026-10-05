//
// Mini force-included header for the design preview: replicates the small
// part of the main build's Skyrim PCH environment that the imguiex sources
// implicitly rely on — <filesystem> visibility (imguiex_m3.h declares
// Initialize(const std::filesystem::path&,...) without including it) and
// unqualified std::min/std::max (the sources call them without a qualifier
// and without windows.h's macros, which the main build's NOMINMAX world
// replaces with using-declarations).
//
#pragma once

#include <algorithm>
#include <filesystem>

using std::max; // NOLINT(misc-header-include-cycle) imguiex assumes these are visible
using std::min;
