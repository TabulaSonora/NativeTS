# The Emscripten web engine: the whole library behind a C API, as an ES module a Web Worker
# imports. The ts_web_* exports come from EMSCRIPTEN_KEEPALIVE in web_api.cpp; only malloc/free
# have to be named here, for the JS side to place the ROM and MIDI bytes.

add_executable(tabulasonora-web
    src/web_api.cpp
    src/web_session.cpp)

target_link_libraries(tabulasonora-web PRIVATE
    ts::tabulasonora
    ts::warnings
    nlohmann_json::nlohmann_json)

set_target_properties(tabulasonora-web PROPERTIES
    OUTPUT_NAME "tabulasonora-engine"
    SUFFIX ".mjs")

# -fwasm-exceptions must match the global compile flag from the preset: the library throws across
# this boundary (RomIdentityError above all), and emcc's default of exceptions-off would turn every
# throw site into an abort.
#
# ENVIRONMENT includes node so the smoke test can drive the module outside a browser. INITIAL_MEMORY
# is sized so the ordinary session — 27 MB ROM, tables, wave cache, and one export's float planes —
# never grows the memory: growth can pause the worker for longer than the pump's lead, which is
# audible as starved frames mid-song. ALLOW_MEMORY_GROWTH stays on as the escape hatch for songs
# longer than the initial size covers.
target_link_options(tabulasonora-web PRIVATE
    -fwasm-exceptions
    "$<$<CONFIG:Release>:-O3>"
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createTabulaSonoraModule
    -sENVIRONMENT=worker,node
    -sINITIAL_MEMORY=268435456
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=1048576
    -sEXPORTED_FUNCTIONS=_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,HEAPF32)

# The Vue app imports the artifacts from its own tree; generated, so gitignored there.
set(TS_WEB_APP_ENGINE_DIR "${PROJECT_SOURCE_DIR}/web/src/engine/generated")

add_custom_command(TARGET tabulasonora-web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TS_WEB_APP_ENGINE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:tabulasonora-web>/tabulasonora-engine.mjs"
        "$<TARGET_FILE_DIR:tabulasonora-web>/tabulasonora-engine.wasm"
        "${TS_WEB_APP_ENGINE_DIR}/"
    COMMENT "Copying the engine module into web/src/engine/generated")
