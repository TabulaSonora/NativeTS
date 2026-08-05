# PROJECT_ rather than CMAKE_ throughout: under add_subdirectory or FetchContent the source root
# belongs to the consuming project, and every one of these paths would resolve into their tree.
include(${PROJECT_SOURCE_DIR}/cmake/EmbedAsset.cmake)

find_package(nlohmann_json CONFIG REQUIRED)

ts_embed_asset(TS_MANIFEST_SOURCE "${PROJECT_SOURCE_DIR}/assets/manifest.json" manifest_json)

add_library(tabulasonora
    dsp/envelope_machine.cpp
    dsp/interpolator.cpp
    dsp/lfo_engine.cpp
    dsp/pitch_chain.cpp
    dsp/segment_envelope.cpp
    dsp/tva_chain.cpp
    dsp/tvf_chain.cpp
    effects/effect_presets.cpp
    effects/effect_programmer.cpp
    effects/equalizer.cpp
    effects/insertion_effect.cpp
    effects/send_effects.cpp
    midi/control_decode.cpp
    midi/midi_formats.cpp
    midi/sequence.cpp
    midi/smf_reader.cpp
    patches/drum_kit_table.cpp
    patches/partial_parameters.cpp
    patches/patch_directory.cpp
    patches/tone.cpp
    dsp/sampler.cpp
    rom/rom_image.cpp
    rom/rom_locator.cpp
    rom/table_set.cpp
    rom/wave_rom.cpp
    rom/sha256.cpp
    rom/table_manifest.cpp
    soundfont/bank_builder.cpp
    soundfont/envelope_fit.cpp
    soundfont/modulators.cpp
    soundfont/sample_set.cpp
    soundfont/writer.cpp
    voices/voice_pool.cpp
    realtime/frame_ring.cpp
    realtime/part.cpp
    realtime/partial_voice.cpp
    realtime/sequence_player.cpp
    realtime/tone_generator.cpp
    realtime/wave_reader.cpp
    render/note_renderer.cpp
    render/render_options.cpp
    render/wav_writer.cpp
    ${TS_MANIFEST_SOURCE}
)

add_library(ts::tabulasonora ALIAS tabulasonora)

# Static unless the consumer asks for shared, and position-independent either way -- a host that
# links the engine into a plugin bundle needs the static archive to be relocatable.
set_target_properties(tabulasonora PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

# CMAKE_CXX_STANDARD governs this project's own compilation and stops at its edge, so the standard
# has to be stated as a usage requirement too. The public headers are C++20 on their face -- and the
# reasons in the README are about semantics the older standards leave undefined, not about syntax --
# so a consumer that compiled them as C++17 would be wrong even where it happened to build.
target_compile_features(tabulasonora PUBLIC cxx_std_20)

target_include_directories(tabulasonora
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

# A static archive carries its PRIVATE link libraries into INTERFACE_LINK_LIBRARIES as
# $<LINK_ONLY:...>, so without the $<BUILD_INTERFACE:> wrapping both of these would become
# requirements of the installed package: importing the engine would demand a warnings target that is
# nobody else's business, and an nlohmann_json that is header-only and already fully consumed at
# build time. Wrapped, they apply while this library compiles and vanish from the export.
target_link_libraries(tabulasonora
    PUBLIC
        ts::numeric_semantics
    PRIVATE
        $<BUILD_INTERFACE:ts::warnings>
        $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)

if(ZLIB_FOUND)
    target_compile_definitions(tabulasonora PRIVATE TS_HAVE_ZLIB)
    target_link_libraries(tabulasonora PRIVATE ZLIB::ZLIB)
endif()

# The generated asset source is machine-written; holding them to the project's warning level
# would fail the build on formatting nobody controls.
set_source_files_properties(${TS_MANIFEST_SOURCE} ${TS_PRESETS_SOURCE}
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")

# The rest of the package -- the headers, the config file, the export set this joins -- is in the
# top-level CMakeLists.txt.
if(TS_INSTALL)
    install(TARGETS tabulasonora
        EXPORT TabulaSonoraTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
