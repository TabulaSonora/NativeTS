find_package(Catch2 3 CONFIG REQUIRED)

add_executable(ts_tests
    effect_programmer_tests.cpp
    effects_tests.cpp
    fixed_tests.cpp
    frame_ring_tests.cpp
    interpolator_tests.cpp
    modulation_tests.cpp
    note_renderer_tests.cpp
    ownership_tests.cpp
    patch_directory_tests.cpp
    port_tests.cpp
    sampler_tests.cpp
    song_oracle_tests.cpp
    wave_codec_tests.cpp
    rom_image_tests.cpp
    sequence_tests.cpp
    sha256_tests.cpp
    table_manifest_tests.cpp
    tone_generator_tests.cpp
    table_set_tests.cpp
    test_data.cpp
    wave_rom_tests.cpp
)

# Nothing Roland-derived is committed, so the tests resolve those assets at run time and skip when
# they are absent. This is where they start looking.
target_compile_definitions(ts_tests PRIVATE TS_REPOSITORY_ROOT="${PROJECT_SOURCE_DIR}")

target_link_libraries(ts_tests
    PRIVATE
        ts::tabulasonora
        ts::numeric_semantics
        ts::warnings
        Catch2::Catch2WithMain)

# The tests reach into the implementation the way the upstream suite does: the vector kernels and
# the fixed-point helpers are internals whose whole claim is that they match a reference, and that
# has to be asserted directly rather than inferred from a render.
target_include_directories(ts_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

include(Catch)
# Tags become CTest labels, so a preset can select a subject rather than guess at test names --
# `ctest -L ring` is how the ThreadSanitizer preset finds the concurrent tests and nothing else.
catch_discover_tests(ts_tests ADD_TAGS_AS_LABELS)

# The one test that is not about the engine: it installs this build and imports it from a separate
# project, so that "another program can link this" is asserted rather than assumed. Carries the
# `package` label, and needs the install rules to exist.
if(TS_INSTALL)
    add_test(NAME package_import
        COMMAND "${CMAKE_COMMAND}"
            "-DTS_BUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DTS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package"
            "-DTS_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-test"
            "-DTS_GENERATOR=${CMAKE_GENERATOR}"
            "-DTS_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DTS_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DTS_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DTS_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DTS_BUILD_TYPE=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/package/RunPackageTest.cmake")

    set_tests_properties(package_import PROPERTIES LABELS package)
endif()
