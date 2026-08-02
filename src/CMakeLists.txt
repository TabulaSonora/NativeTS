include(${CMAKE_SOURCE_DIR}/cmake/EmbedAsset.cmake)

find_package(nlohmann_json CONFIG REQUIRED)

ts_embed_asset(TS_MANIFEST_SOURCE "${CMAKE_SOURCE_DIR}/assets/manifest.json" manifest_json)
ts_embed_asset(TS_PRESETS_SOURCE  "${CMAKE_SOURCE_DIR}/assets/presets.json"  presets_json)

add_library(tabulasonora
    rom/rom_image.cpp
    rom/table_set.cpp
    rom/wave_rom.cpp
    rom/sha256.cpp
    rom/table_manifest.cpp
    ${TS_MANIFEST_SOURCE}
    ${TS_PRESETS_SOURCE}
)

add_library(ts::tabulasonora ALIAS tabulasonora)

target_include_directories(tabulasonora
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tabulasonora
    PUBLIC
        ts::numeric_semantics
    PRIVATE
        ts::warnings
        nlohmann_json::nlohmann_json)

# The generated asset sources are machine-written; holding them to the project's warning level
# would fail the build on formatting nobody controls.
set_source_files_properties(${TS_MANIFEST_SOURCE} ${TS_PRESETS_SOURCE}
    PROPERTIES COMPILE_OPTIONS "-Wno-everything")
