find_package(CLI11 CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)

add_executable(tabula-sonora-tui main.cpp)

target_link_libraries(tabula-sonora-tui
    PRIVATE
        ts::audio
        ts::numeric_semantics
        ts::warnings
        # component already carries dom and screen; naming all three makes the linker warn
        # about duplicates on macOS.
        ftxui::component
        CLI11::CLI11)

set_target_properties(tabula-sonora-tui PROPERTIES OUTPUT_NAME tabula-sonora-tui)
