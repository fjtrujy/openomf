cmake_minimum_required(VERSION 3.16)
project(OpenOMF_PSP C)

# Configure with psp-cmake; this is deliberately independent of the desktop
# OpenGL/epoxy build so both platforms keep their own dependencies.
if(NOT PSP)
    message(FATAL_ERROR "Use psp-cmake to configure this port")
endif()

get_filename_component(OMF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
option(PSP_GU_PROFILE "Log native GU GPU/CPU-effect frame counts every 600 frames" OFF)
option(PSP_CLOCK_333 "Use PSP's supported 333 MHz CPU clock for full-speed gameplay (restored at exit)" ON)
option(PSP_NATIVE_INPUT "Read PSP controls through sceCtrl and skip SDL input events" ON)
option(PSP_NATIVE_INPUT_TRACE "Log PSP physical controller state changes for regression tests" OFF)
option(PSP_NATIVE_TIMING "Use sceKernelGetSystemTimeWide and sceKernelDelayThread for PSP game/frame timing" ON)
option(PSP_NATIVE_DISPLAY "Initialize PSP GU display without SDL video or an SDL window" ON)
option(PSP_TEST_SOFTWARE_RENDERER "Force the SDL software renderer on PSP for isolated fallback testing" OFF)
option(PSP_STANDALONE "Build PSP without linking SDL2 or SDL2main" ON)
option(PSP_NATIVE_AUDIO "Use the native sceAudio backend (no SDL_mixer dependency by default)" ON)
option(PSP_AUDIO_MIX_DIAGNOSTICS "Count native PSP mixer clipping, peaks and output timing" OFF)
option(PSP_SDL_AUDIO_FALLBACK "Also compile SDL_mixer as an alternate PSP audio backend" OFF)
if(PSP_STANDALONE AND (NOT PSP_NATIVE_INPUT OR NOT PSP_NATIVE_TIMING OR NOT PSP_NATIVE_AUDIO OR
                       NOT PSP_NATIVE_DISPLAY OR PSP_SDL_AUDIO_FALLBACK))
    message(FATAL_ERROR "PSP_STANDALONE requires native input, timing, audio, display and no SDL_mixer fallback")
endif()
option(PSP_STRESS_DEMO "Start AI-vs-AI arena stress test for profiling (never enable in a shipping build)" OFF)
set(PSP_STRESS_ARENA 0 CACHE STRING "Arena 0..4 for the 120s PSP stress test")
if(NOT PSP_STRESS_ARENA MATCHES "^[0-4]$")
    message(FATAL_ERROR "PSP_STRESS_ARENA must be an integer from 0 through 4")
endif()
option(PSP_GU_APPROX_EFFECTS "Fast GPU alpha approximation for destination-index effects (not pixel-exact)" ON)

file(GLOB_RECURSE OMF_CORE CONFIGURE_DEPENDS "${OMF_ROOT}/src/*/*.c")
list(FILTER OMF_CORE EXCLUDE REGEX "/src/audio/backends/")
list(FILTER OMF_CORE EXCLUDE REGEX "/src/video/renderers/")
list(FILTER OMF_CORE EXCLUDE REGEX "/src/vendored/(argtable|zip)/")
list(REMOVE_ITEM OMF_CORE "${OMF_ROOT}/src/controller/psp_joystick.c")
list(APPEND OMF_CORE
    "${OMF_ROOT}/src/video/renderers/psp/gu_renderer.c"
)
if(NOT PSP_STANDALONE)
    list(APPEND OMF_CORE "${OMF_ROOT}/src/video/renderers/software/software_renderer.c")
endif()
if(PSP_STANDALONE)
    list(APPEND OMF_CORE "${CMAKE_CURRENT_SOURCE_DIR}/psp_sdl_compat.c"
                         "${CMAKE_CURRENT_SOURCE_DIR}/psp_lifecycle.c")
endif()
if(PSP_NATIVE_INPUT)
    list(REMOVE_ITEM OMF_CORE "${OMF_ROOT}/src/controller/joystick.c")
    list(REMOVE_ITEM OMF_CORE "${OMF_ROOT}/src/controller/game_controller_db.c")
    list(APPEND OMF_CORE "${OMF_ROOT}/src/controller/psp_joystick.c")
endif()
if(PSP_NATIVE_AUDIO)
    list(APPEND OMF_CORE "${OMF_ROOT}/src/audio/backends/psp/psp_backend.c")
endif()
if(NOT PSP_NATIVE_AUDIO OR PSP_SDL_AUDIO_FALLBACK)
    list(APPEND OMF_CORE "${OMF_ROOT}/src/audio/backends/sdl/sdl_backend.c")
endif()

add_executable(openomf_psp
    "${OMF_ROOT}/src/main.c"
    "${OMF_ROOT}/src/engine.c"
    ${OMF_CORE}
)

# The extension language (.DAT2) is not part of the original freeware archive.
# Build its one-entry catalog with a host C compiler; the full desktop
# languagetool requires unrelated desktop SDL_mixer/OpenGL dependencies.
find_program(OMF_HOST_CC NAMES cc clang PATHS /usr/bin /opt/homebrew/bin NO_DEFAULT_PATH REQUIRED)
set(OMF_LANG2_GENERATOR "${CMAKE_CURRENT_BINARY_DIR}/create_language2_host")
set(OMF_LANG2_FILE "${CMAKE_CURRENT_BINARY_DIR}/resources/ENGLISH.DAT2")
add_custom_command(OUTPUT "${OMF_LANG2_FILE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/resources"
    COMMAND "${OMF_HOST_CC}" -std=c11 -O2
      "${CMAKE_CURRENT_SOURCE_DIR}/create_language2.c" -o "${OMF_LANG2_GENERATOR}"
    COMMAND "${OMF_LANG2_GENERATOR}" "${OMF_ROOT}/resources/ENGLISH2.TXT" "${OMF_LANG2_FILE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/create_language2.c" "${OMF_ROOT}/resources/ENGLISH2.TXT"
    COMMENT "Generate OpenOMF English extension language data on the host"
)
add_custom_target(omf_psp_language DEPENDS "${OMF_LANG2_FILE}")
add_dependencies(openomf_psp omf_psp_language)
target_include_directories(openomf_psp PRIVATE
    "${OMF_ROOT}/src"
    "${PSPDEV}/psp/sdk/include"
)
if(NOT PSP_STANDALONE)
    target_include_directories(openomf_psp PRIVATE "${PSPDEV}/psp/include/SDL2")
endif()
# CMake sometimes treats the SDK headers as an implicit compiler include
# directory and drops the -I even though this cross GCC does not search it.
target_compile_options(openomf_psp PRIVATE "-I${PSPDEV}/psp/sdk/include")
target_compile_definitions(openomf_psp PRIVATE
    ENABLE_PSP_GU_RENDERER
    ENABLE_SOFTWARE_RENDERER
    PNG_FOUND
    V_MAJOR=0 V_MINOR=0 V_PATCH=0 V_LABEL="PSP"
)
if(PSP_GU_PROFILE)
    target_compile_definitions(openomf_psp PRIVATE PSP_GU_PROFILE)
endif()
if(PSP_CLOCK_333)
    target_compile_definitions(openomf_psp PRIVATE PSP_CLOCK_333)
endif()
if(PSP_NATIVE_INPUT)
    target_compile_definitions(openomf_psp PRIVATE PSP_NATIVE_INPUT)
endif()
if(PSP_NATIVE_INPUT AND PSP_NATIVE_INPUT_TRACE)
    target_compile_definitions(openomf_psp PRIVATE PSP_NATIVE_INPUT_TRACE)
endif()
if(PSP_NATIVE_TIMING)
    target_compile_definitions(openomf_psp PRIVATE PSP_NATIVE_TIMING)
endif()
if(PSP_NATIVE_DISPLAY)
    target_compile_definitions(openomf_psp PRIVATE PSP_NATIVE_DISPLAY)
endif()
if(PSP_STANDALONE)
    target_compile_definitions(openomf_psp PRIVATE PSP_STANDALONE SDL_MAIN_HANDLED)
endif()
if(PSP_TEST_SOFTWARE_RENDERER)
    target_compile_definitions(openomf_psp PRIVATE PSP_TEST_SOFTWARE_RENDERER)
endif()
if(PSP_NATIVE_AUDIO)
    target_compile_definitions(openomf_psp PRIVATE ENABLE_PSP_AUDIO_BACKEND)
endif()
if(PSP_NATIVE_AUDIO AND PSP_AUDIO_MIX_DIAGNOSTICS)
    target_compile_definitions(openomf_psp PRIVATE PSP_AUDIO_MIX_DIAGNOSTICS)
endif()
if(NOT PSP_NATIVE_AUDIO OR PSP_SDL_AUDIO_FALLBACK)
    target_compile_definitions(openomf_psp PRIVATE ENABLE_SDL_AUDIO_BACKEND)
endif()
if(PSP_GU_APPROX_EFFECTS)
    target_compile_definitions(openomf_psp PRIVATE PSP_GU_APPROX_EFFECTS)
endif()
if(PSP_STRESS_DEMO)
    target_compile_definitions(openomf_psp PRIVATE
        PSP_STRESS_DEMO PSP_GU_PROFILE PSP_STRESS_ARENA=${PSP_STRESS_ARENA})
endif()

# SDL2's PSP backend uses the GU for display; no OpenGL 3.3 or libepoxy.
# With static archives, SDL_mixer must precede its dependencies at link time.
set(PSP_AUDIO_LIBRARIES "")
if(NOT PSP_NATIVE_AUDIO OR PSP_SDL_AUDIO_FALLBACK)
    list(APPEND PSP_AUDIO_LIBRARIES SDL2_mixer)
endif()
if(NOT PSP_STANDALONE)
    list(APPEND PSP_AUDIO_LIBRARIES SDL2main SDL2)
endif()
target_link_libraries(openomf_psp PRIVATE
    ${PSP_AUDIO_LIBRARIES} xmp enet
    png16
    GL pspvram pspvfpu psphprm
    vorbisfile vorbis ogg modplug stdc++
    pspgu pspge pspdisplay pspctrl pspaudio pspnet pspnet_inet
    pspnet_resolver pspnet_apctl pspwlan psppower
    pspdebug
    m z
)
create_pbp_file(TARGET openomf_psp TITLE "OpenOMF PSP"
    ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon0.png"
    MEMSIZE 1)

add_custom_command(TARGET openomf_psp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OMF_ROOT}/resources/openomf.bk"
      "${CMAKE_CURRENT_BINARY_DIR}/resources/openomf.bk"
    COMMENT "Copy OpenOMF's bundled resource index"
)
