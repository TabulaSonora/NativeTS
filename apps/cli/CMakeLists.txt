find_package(CLI11 CONFIG REQUIRED)

add_executable(tabula-sonora main.cpp)

target_link_libraries(tabula-sonora
    PRIVATE
        ts::tabulasonora
        ts::numeric_semantics
        ts::warnings
        CLI11::CLI11)

set_target_properties(tabula-sonora PROPERTIES OUTPUT_NAME tabula-sonora)
