find_package(Catch2 3 CONFIG REQUIRED)

add_executable(ts_tests
    effects_tests.cpp
    fixed_tests.cpp
    interpolator_tests.cpp
    modulation_tests.cpp
    note_renderer_tests.cpp
    patch_directory_tests.cpp
    sampler_tests.cpp
    wave_codec_tests.cpp
    rom_image_tests.cpp
    sequence_renderer_tests.cpp
    sequence_tests.cpp
    sha256_tests.cpp
    table_manifest_tests.cpp
    table_set_tests.cpp
    test_data.cpp
    wave_rom_tests.cpp
)

# Nothing Roland-derived is committed, so the tests resolve those assets at run time and skip when
# they are absent. This is where they start looking.
target_compile_definitions(ts_tests PRIVATE TS_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")

target_link_libraries(ts_tests
    PRIVATE
        ts::tabulasonora
        ts::numeric_semantics
        ts::warnings
        Catch2::Catch2WithMain)

# The tests reach into the implementation the way the upstream suite does: the vector kernels and
# the fixed-point helpers are internals whose whole claim is that they match a reference, and that
# has to be asserted directly rather than inferred from a render.
target_include_directories(ts_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(Catch)
catch_discover_tests(ts_tests)
