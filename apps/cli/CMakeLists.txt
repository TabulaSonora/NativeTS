find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(tabula-sonora main.cpp)

target_link_libraries(tabula-sonora
    PRIVATE
        ts::tabulasonora
        ts::numeric_semantics
        ts::warnings
        CLI11::CLI11
        nlohmann_json::nlohmann_json)

set_target_properties(tabula-sonora PROPERTIES OUTPUT_NAME tabula-sonora)
