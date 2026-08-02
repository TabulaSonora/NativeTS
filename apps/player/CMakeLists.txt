find_package(CLI11 CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

# miniaudio compiled alone, and deliberately without ts::warnings. It is a vendored single-header
# library: its warnings are not this project's to fix, and silencing them target-wide would silence
# them for the player's own code too.
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

add_executable(tabula-sonora-play
    main.cpp
    sources.cpp
    terminal.cpp)

target_link_libraries(tabula-sonora-play
    PRIVATE
        ts::tabulasonora
        ts::numeric_semantics
        ts::warnings
        ts_miniaudio
        CLI11::CLI11)

set_target_properties(tabula-sonora-play PROPERTIES OUTPUT_NAME tabula-sonora-play)
