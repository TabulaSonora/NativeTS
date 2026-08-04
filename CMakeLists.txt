cmake_minimum_required(VERSION 3.24)

# The vcpkg toolchain has to be in scope before project(). CMakePresets.json sets it; this
# fallback picks up a VCPKG_ROOT from the environment so a bare `cmake -B build` also works.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

# Say which file is missing, and stop. A toolchain file that does not exist takes project() down
# before platform detection, and what CMake prints then is "unable to find a build program
# corresponding to Ninja" -- an accusation against a build tool that is installed and innocent. The
# preset spells the path as $env{VCPKG_ROOT}/scripts/..., so an unset VCPKG_ROOT arrives here as a
# path rooted at /, which is the usual way to end up in that dead end.
if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    # Say which toolchain, not just which variable. The web presets point at Emscripten's file and
    # the rest at vcpkg's, and reporting VCPKG_ROOT for a missing Emscripten toolchain sends the
    # reader to the wrong tool entirely -- which it did.
    if(CMAKE_TOOLCHAIN_FILE MATCHES "[Ee]mscripten")
        if("$ENV{EMSDK}" STREQUAL "")
            # One argument, not four: set() with several strings makes a list, and CMake joins a
            # list with semicolons when it interpolates one into a message.
            string(CONCAT ts_toolchain_state
                "EMSDK is not set. Under an emsdk checkout, source its emsdk_env.sh first. For a "
                "distribution package EMSDK does not apply -- use the `web-system` preset, or pass "
                "-DCMAKE_TOOLCHAIN_FILE=<prefix>/cmake/Modules/Platform/Emscripten.cmake yourself")
        else()
            set(ts_toolchain_state "EMSDK is '$ENV{EMSDK}'")
        endif()
    elseif("$ENV{VCPKG_ROOT}" STREQUAL "")
        set(ts_toolchain_state "VCPKG_ROOT is not set in the environment")
    else()
        set(ts_toolchain_state "VCPKG_ROOT is '$ENV{VCPKG_ROOT}'")
    endif()

    message(FATAL_ERROR
        "No toolchain file at '${CMAKE_TOOLCHAIN_FILE}'.\n"
        "${ts_toolchain_state}; see the build instructions in README.md.\n"
        "Delete the build directory before retrying -- a configure that fails here leaves a cache "
        "that fails differently next time.")
endif()

project(NativeTabulaSonora
    VERSION 0.1.0
    DESCRIPTION "A native implementation of the Roland Sound Canvas VA synth voice"
    LANGUAGES CXX)

# Before the targets: the install layout variables are read while the library's usage requirements
# are being written, not only when the install rules run.
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The front ends and the test suite are for people working on the engine; a project that embeds it
# wants the library and nothing else. So the defaults follow PROJECT_IS_TOP_LEVEL: building this
# repository directly gets the tests and the CLI as before, while an add_subdirectory or FetchContent
# consumer gets only the library and is not asked for Catch2 or CLI11.
option(TS_BUILD_TESTS  "Build the test suite"          ${PROJECT_IS_TOP_LEVEL})
option(TS_BUILD_CLI    "Build the tabula-sonora CLI"   ${PROJECT_IS_TOP_LEVEL})
option(TS_BUILD_PLAYER "Build the terminal player"     OFF)
option(TS_BUILD_TUI    "Build the full-screen TUI"     OFF)
option(TS_BUILD_WEB    "Build the Emscripten web engine module" OFF)
option(TS_BUILD_DOCS   "Add the Doxygen documentation target"   OFF)
option(TS_INSTALL      "Generate the install and package rules" ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------------------------
# Numeric semantics. These are not tuning knobs -- they are correctness requirements.
#
# The engine's control path is 16-bit fixed point and several expressions depend on wrapping and
# on truncation direction. The upstream C# build disables overflow checking project-wide and
# records that this is load-bearing, not an optimisation.
#
#   -fwrapv               signed overflow wraps instead of being UB. Belt-and-braces: the
#                         intentional-overflow expressions also go through the helpers in
#                         dsp/fixed.hpp, so the code stays correct without it.
#   -ffp-contract=off     clang defaults to `fast` and will fuse a*b+c into an FMA. That breaks
#                         every float/double narrowing guarantee the DSP rests on, and with it
#                         the claim that a vector kernel is bit-identical to its scalar loop.
#
# Never add -ffast-math. The block loop relies on exact signed-zero behaviour ("a sum that starts
# at +0 can never reach -0"), which -ffast-math discards.
# ---------------------------------------------------------------------------------------------
add_library(ts_numeric_semantics INTERFACE)
add_library(ts::numeric_semantics ALIAS ts_numeric_semantics)

# Without this the export set would name it ts::ts_numeric_semantics, and the target an importing
# project sees would differ from the one an add_subdirectory consumer sees.
set_target_properties(ts_numeric_semantics PROPERTIES EXPORT_NAME numeric_semantics)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ts_numeric_semantics INTERFACE
        -fwrapv
        -ffp-contract=off)
elseif(MSVC)
    # No -ffp-contract=off equivalent needed: /fp:precise leaves contraction off unless /fp:contract
    # is passed explicitly. Never add /fp:contract or /fp:fast here.
    target_compile_options(ts_numeric_semantics INTERFACE
        /fp:precise)
endif()

add_library(ts_warnings INTERFACE)
add_library(ts::warnings ALIAS ts_warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ts_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow
        -Wconversion -Wsign-conversion
        -Wold-style-cast
        -Wdouble-promotion
        -Wnon-virtual-dtor)
elseif(MSVC)
    target_compile_options(ts_warnings INTERFACE /W4 /permissive-)
endif()

# The web build runs under the Emscripten toolchain, where vcpkg is not in play. The one base
# dependency is header-only, so it is fetched directly; OVERRIDE_FIND_PACKAGE satisfies the
# find_package(nlohmann_json) in src/ without touching it.
if(TS_BUILD_WEB AND EMSCRIPTEN)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        OVERRIDE_FIND_PACKAGE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_subdirectory(src)

if(TS_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(TS_BUILD_PLAYER OR TS_BUILD_TUI)
    add_subdirectory(apps/audio)
endif()

if(TS_BUILD_PLAYER)
    add_subdirectory(apps/player)
endif()

if(TS_BUILD_TUI)
    add_subdirectory(apps/tui)
endif()

if(TS_BUILD_WEB)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "TS_BUILD_WEB requires the Emscripten toolchain; use the 'web' preset.")
    endif()
    add_subdirectory(apps/web)
endif()

if(TS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------------------------
# Documentation.
#
# The docs do not really belong to the build: `doxygen docs/Doxyfile`, run from the source root, is
# the canonical way to produce them, and it needs neither a compiler, nor vcpkg, nor SCCore.dll --
# which is what lets CI render the site from a clean clone. This target only spares an editor the
# trouble of knowing that, so it shells out to exactly the same command.
#
# The output therefore lands in <source>/build/docs as the Doxyfile says, not under this preset's
# binary directory.
# ---------------------------------------------------------------------------------------------
if(TS_BUILD_DOCS)
    find_package(Doxygen 1.9.8 REQUIRED)

    add_custom_target(docs
        COMMAND Doxygen::doxygen "${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Doxygen -> build/docs"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------------------------
# Installing the engine as a package another project can import.
#
# The point of this port is that a host embeds the engine, so `ts::tabulasonora` is the deliverable
# and the front ends are demonstrations of it. Two ways in, and both hand back the same target name:
#
#   find_package(TabulaSonora REQUIRED)        # against an installed tree
#   add_subdirectory(NativeTS)                 # or FetchContent, in-tree
#   target_link_libraries(host PRIVATE ts::tabulasonora)
#
# ts::numeric_semantics rides along in the export set because it is a PUBLIC dependency: the inline
# DSP in the public headers is only correct when compiled with FMA contraction off. ts::warnings does
# not, and must not -- see the $<BUILD_INTERFACE:> wrapping in src/CMakeLists.txt.
# ---------------------------------------------------------------------------------------------
if(TS_INSTALL)
    include(CMakePackageConfigHelpers)

    set(TS_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/TabulaSonora"
        CACHE STRING "Where TabulaSonoraConfig.cmake is installed")

    install(TARGETS ts_numeric_semantics EXPORT TabulaSonoraTargets)

    install(DIRECTORY include/tabulasonora
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.hpp")

    install(EXPORT TabulaSonoraTargets
        FILE TabulaSonoraTargets.cmake
        NAMESPACE ts::
        DESTINATION "${TS_INSTALL_CMAKEDIR}")

    configure_package_config_file(
        cmake/TabulaSonoraConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/TabulaSonoraConfig.cmake"
        INSTALL_DESTINATION "${TS_INSTALL_CMAKEDIR}")

    # Pre-1.0, a minor bump is where the engine's API is allowed to move, so that is the
    # compatibility boundary rather than the major version.
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/TabulaSonoraConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/TabulaSonoraConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/TabulaSonoraConfigVersion.cmake"
        DESTINATION "${TS_INSTALL_CMAKEDIR}")

    # NOTICE.md travels with the binary on purpose. The library carries the reverb and chorus
    # coefficients compiled in, those are Roland-derived, and a host that links the engine inherits
    # the terms that file sets out.
    install(FILES LICENSE NOTICE.md
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/TabulaSonora")
endif()
