cmake_minimum_required(VERSION 3.24)

# The vcpkg toolchain has to be in scope before project(). CMakePresets.json sets it; this
# fallback picks up a VCPKG_ROOT from the environment so a bare `cmake -B build` also works.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

project(NativeTabulaSonora
    VERSION 0.1.0
    DESCRIPTION "A native implementation of the Roland Sound Canvas VA synth voice"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TS_BUILD_TESTS  "Build the test suite"          ON)
option(TS_BUILD_CLI    "Build the tabula-sonora CLI"   ON)
option(TS_BUILD_PLAYER "Build the terminal player"     OFF)

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

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ts_numeric_semantics INTERFACE
        -fwrapv
        -ffp-contract=off)
elseif(MSVC)
    target_compile_options(ts_numeric_semantics INTERFACE
        /fp:precise
        /fp:contract-)
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

add_subdirectory(src)

if(TS_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(TS_BUILD_PLAYER)
    add_subdirectory(apps/player)
endif()

if(TS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
