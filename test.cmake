enable_testing()

file(GLOB_RECURSE TEST_SOURCES test/*.cpp)
file(GLOB_RECURSE Benchmark_TEST_SOURCES benchmarks/*.cpp)

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

add_executable(
    ${TEST_PROJ_NAME}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/configs/ConfigSerializer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/configs/settings_converter.cpp"
    "${JAMIE_MODS_COMMON_DIR}/i18n/translator_manager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ime/TextEditor.cpp"
    ${TEST_SOURCES}
)

# Benchmark target configuration.
#
# NOTE: Local scratch-pads or transient design-phase tests should remain uncommitted.
# To include permanent benchmarks in the build, append your source files to the
# `Benchmark_TEST_SOURCES` list.

if (Benchmark_TEST_SOURCES)
    add_executable(
        ${PROJECT_NAME}Benchmark
        ${Benchmark_TEST_SOURCES}
    )
    target_link_libraries(
        ${PROJECT_NAME}Benchmark
        PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
        spdlog::spdlog
        benchmark::benchmark
    )
    target_compile_features(${PROJECT_NAME}Benchmark PRIVATE cxx_std_23)
    target_include_directories(
        ${PROJECT_NAME}Benchmark
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
else ()
    message(WARNING "No benchmark sources found. Skipping benchmark target creation.")
endif ()
#target_compile_options(${TEST_PROJ_NAME} PRIVATE -fsanitize=address /Zi -D_DISABLE_STRING_ANNOTATION -D_DISABLE_VECTOR_ANNOTATION  -g )
target_link_libraries(
    ${TEST_PROJ_NAME}
    PRIVATE
    GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    spdlog::spdlog
    ${DETOURS_LIBRARY}
    Freetype::Freetype
)

target_compile_definitions(${TEST_PROJ_NAME} PRIVATE SIMPLE_IME="SimpleIME")
target_compile_features(${TEST_PROJ_NAME} PRIVATE cxx_std_23)
# Mirror the main target's include layout: the shared helpers and imguiex now live
# in the JamieMods submodule, so the old ${CMAKE_SOURCE_DIR}/common and /imguiex
# paths no longer resolve.
target_include_directories(
    ${TEST_PROJ_NAME}
    PRIVATE
    ${JAMIE_MODS_DIR}/common
    ${JAMIE_MODS_EXTERN_DIR}
    ${IMGUIEX_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${EXTERN_DIR} # vendored UI-framework SDK headers (MeridianUI/) for the bridge ABI tests
    ${IMGUI_INCLUDE_DIRS}
)

include(GoogleTest)
gtest_discover_tests(${TEST_PROJ_NAME})

# The shipped default config feeds ShippedConfigurationTest: the shipped file
# must convert to exactly the compiled-in default Settings.
add_custom_command(
    TARGET "${TEST_PROJ_NAME}"
    PRE_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/contrib/config/SimpleIME.toml" "$<TARGET_FILE_DIR:${TEST_PROJ_NAME}>/SimpleIME.toml"
    VERBATIM
)
