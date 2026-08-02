find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

# miniaudio compiled alone, and deliberately without ts::warnings. It is a vendored single-header
# library: its warnings are not this project's to fix, and silencing them target-wide would silence
# them for this project's own code too.
add_library(ts_miniaudio STATIC miniaudio.cpp)
target_include_directories(ts_miniaudio SYSTEM PUBLIC "${MINIAUDIO_INCLUDE_DIR}")
target_compile_options(ts_miniaudio PRIVATE -w)

if(APPLE)
    target_link_libraries(ts_miniaudio
        PUBLIC
            "-framework CoreAudio"
            "-framework CoreFoundation"
            "-framework AudioToolbox")
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ts_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Everything a front end needs to turn a MIDI file into sound: the sources, the device, the render
# thread and the transport. Both the line player and the TUI drive this rather than each opening a
# device of its own -- the ring protocol and the pause-by-stopping-the-device rule are exactly the
# things that should exist once.
add_library(ts_audio STATIC
    playback.cpp
    sources.cpp)

target_include_directories(ts_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(ts_audio
    PUBLIC
        ts::tabulasonora
        ts_miniaudio
    PRIVATE
        ts::numeric_semantics
        ts::warnings)

add_library(ts::audio ALIAS ts_audio)
