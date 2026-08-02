find_package(Catch2 3 CONFIG REQUIRED)

add_executable(ts_tests
    fixed_tests.cpp
    table_manifest_tests.cpp
)

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
