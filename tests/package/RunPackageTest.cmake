# Drives the package consumer end to end: install this build into a scratch prefix, then configure,
# build and run tests/package as a project that has never heard of this source tree.
#
# It is a test rather than a documented ritual because the ways an export set breaks are all silent
# at build time -- an include directory that still points into the source tree, a private dependency
# that leaked into the interface, a header the install rules forgot. Every one of those configures
# fine here and fails in somebody else's project.
#
# Expects: TS_BUILD_DIR, TS_SOURCE_DIR, TS_WORK_DIR, TS_CXX_COMPILER, TS_BUILD_TYPE, and the
# generator description -- TS_GENERATOR, TS_MAKE_PROGRAM, TS_GENERATOR_PLATFORM, TS_GENERATOR_TOOLSET.

cmake_minimum_required(VERSION 3.24)

foreach(required TS_BUILD_DIR TS_SOURCE_DIR TS_WORK_DIR TS_CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "RunPackageTest.cmake: ${required} was not set")
    endif()
endforeach()

set(prefix "${TS_WORK_DIR}/prefix")
set(binary "${TS_WORK_DIR}/consumer-build")

# Start clean, so a stale prefix from a previous run cannot satisfy the find_package.
file(REMOVE_RECURSE "${TS_WORK_DIR}")
file(MAKE_DIRECTORY "${TS_WORK_DIR}")

function(ts_run description)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "${description} failed (exit ${code})")
    endif()
endfunction()

ts_run("installing the engine"
    "${CMAKE_COMMAND}" --install "${TS_BUILD_DIR}" --prefix "${prefix}" --config "${TS_BUILD_TYPE}")

# The generator has to be described to the child the way the parent had it described, not merely
# named. "Ninja" is a name CMake still has to resolve to a binary, and it resolves it off PATH: a
# ninja that came from an IDE, a toolchain or a preset's CMAKE_MAKE_PROGRAM is not on the PATH a test
# inherits, and the child configure fails with "unable to find a build program corresponding to
# Ninja". Platform and toolset are here for the same reason, for the generators that take them.
macro(ts_forward name value)
    if(NOT "${value}" STREQUAL "")
        list(APPEND generator_args "-D${name}=${value}")
    endif()
endmacro()

set(generator_args -G "${TS_GENERATOR}")
ts_forward(CMAKE_MAKE_PROGRAM       "${TS_MAKE_PROGRAM}")
ts_forward(CMAKE_GENERATOR_PLATFORM "${TS_GENERATOR_PLATFORM}")
ts_forward(CMAKE_GENERATOR_TOOLSET  "${TS_GENERATOR_TOOLSET}")

# No toolchain file and no CMAKE_PREFIX_PATH beyond the install itself: if the exported target still
# wanted nlohmann_json, or ts::warnings, this is where it would say so.
ts_run("configuring the consumer"
    "${CMAKE_COMMAND}"
        -S "${TS_SOURCE_DIR}"
        -B "${binary}"
        ${generator_args}
        "-DCMAKE_PREFIX_PATH=${prefix}"
        "-DCMAKE_CXX_COMPILER=${TS_CXX_COMPILER}"
        "-DCMAKE_BUILD_TYPE=${TS_BUILD_TYPE}")

ts_run("building the consumer"
    "${CMAKE_COMMAND}" --build "${binary}" --config "${TS_BUILD_TYPE}")

find_program(consumer_exe consumer PATHS "${binary}" "${binary}/${TS_BUILD_TYPE}" NO_DEFAULT_PATH)
if(NOT consumer_exe)
    message(FATAL_ERROR "the consumer built but no executable was found under ${binary}")
endif()

ts_run("running the consumer" "${consumer_exe}")
