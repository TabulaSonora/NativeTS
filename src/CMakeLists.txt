include(${CMAKE_SOURCE_DIR}/cmake/EmbedAsset.cmake)

find_package(nlohmann_json CONFIG REQUIRED)

ts_embed_asset(TS_MANIFEST_SOURCE "${CMAKE_SOURCE_DIR}/assets/manifest.json" manifest_json)
ts_embed_asset(TS_PRESETS_SOURCE  "${CMAKE_SOURCE_DIR}/assets/presets.json"  presets_json)

add_library(tabulasonora
    dsp/envelope_machine.cpp
    dsp/interpolator.cpp
    dsp/lfo_engine.cpp
    dsp/pitch_chain.cpp
    dsp/segment_envelope.cpp
    dsp/tva_chain.cpp
    dsp/tvf_chain.cpp
    effects/effect_presets.cpp
    effects/send_effects.cpp
    midi/sequence.cpp
    midi/smf_reader.cpp
    patches/drum_kit_table.cpp
    patches/partial_parameters.cpp
    patches/patch_directory.cpp
    patches/tone.cpp
    dsp/sampler.cpp
    rom/rom_image.cpp
    rom/table_set.cpp
    rom/wave_rom.cpp
    rom/sha256.cpp
    rom/table_manifest.cpp
    render/note_renderer.cpp
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
