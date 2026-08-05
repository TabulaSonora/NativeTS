# Finds Emscripten's own toolchain file and hands over to it, so the `web` preset works against
# whichever Emscripten the machine happens to have. The upstream file lives at a different depth in
# every distribution: an emsdk checkout puts it under $EMSDK/upstream/emscripten, Homebrew under the
# formula's libexec, Debian and Arch under a plain /usr/lib/emscripten. Pointing the preset at one
# of those spellings is what forced the web build onto a single machine.
#
# Resolution order: $EMSDK if it is set and real, then the layouts around whichever emcc is on PATH,
# then emcc's own answer. Override the whole thing by passing -DCMAKE_TOOLCHAIN_FILE explicitly.
#
# CMake re-reads a toolchain file for every try_compile, so this stays cheap: two file tests, and
# the em-config subprocess only on the path where the guesses all missed.

set(_ts_emscripten_toolchain "")

# The suffix below the emscripten root is the one part every distribution agrees on.
set(_ts_emscripten_suffix "cmake/Modules/Platform/Emscripten.cmake")

if(DEFINED ENV{EMSDK} AND EXISTS "$ENV{EMSDK}/upstream/emscripten/${_ts_emscripten_suffix}")
    set(_ts_emscripten_toolchain "$ENV{EMSDK}/upstream/emscripten/${_ts_emscripten_suffix}")
endif()

if(NOT _ts_emscripten_toolchain)
    find_program(_ts_emcc emcc)
    if(_ts_emcc)
        # Through the symlink first: /opt/homebrew/bin/emcc is a link into the Cellar, and only the
        # resolved path sits next to the libexec the cmake modules live in.
        file(REAL_PATH "${_ts_emcc}" _ts_emcc_real)
        get_filename_component(_ts_emcc_dir "${_ts_emcc_real}" DIRECTORY)

        # emsdk and the Linux distribution packages: emcc sits in the emscripten root itself.
        # Homebrew: emcc is a shell wrapper in bin/, one level up from the real root in libexec/.
        foreach(_ts_candidate "${_ts_emcc_dir}" "${_ts_emcc_dir}/../libexec")
            if(NOT _ts_emscripten_toolchain AND EXISTS "${_ts_candidate}/${_ts_emscripten_suffix}")
                file(REAL_PATH "${_ts_candidate}/${_ts_emscripten_suffix}" _ts_emscripten_toolchain)
            endif()
        endforeach()

        # Last resort: ask emcc where it thinks it lives. Costs a Python start-up, so it only runs
        # for a layout neither guess above covers.
        if(NOT _ts_emscripten_toolchain)
            execute_process(COMMAND "${_ts_emcc_dir}/em-config" EMSCRIPTEN_ROOT
                OUTPUT_VARIABLE _ts_emscripten_root
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_ts_emscripten_root AND EXISTS "${_ts_emscripten_root}/${_ts_emscripten_suffix}")
                set(_ts_emscripten_toolchain "${_ts_emscripten_root}/${_ts_emscripten_suffix}")
            endif()
        endif()
    endif()
endif()

if(NOT _ts_emscripten_toolchain)
    message(FATAL_ERROR
        "No Emscripten toolchain found. Install the SDK and put emcc on PATH (or set EMSDK to an "
        "emsdk checkout), then configure again. To point at one directly, pass "
        "-DCMAKE_TOOLCHAIN_FILE=<path>/cmake/Modules/Platform/Emscripten.cmake.")
endif()

include("${_ts_emscripten_toolchain}")
