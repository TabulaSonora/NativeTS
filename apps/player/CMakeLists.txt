find_package(CLI11 CONFIG REQUIRED)

add_executable(tabula-sonora-play
    main.cpp
    terminal.cpp)

target_link_libraries(tabula-sonora-play
    PRIVATE
        ts::audio
        ts::numeric_semantics
        ts::warnings
        CLI11::CLI11)

set_target_properties(tabula-sonora-play PROPERTIES OUTPUT_NAME tabula-sonora-play)
