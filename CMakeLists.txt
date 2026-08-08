cmake_minimum_required(VERSION 3.14)

# Honor CMAKE_INTERPROCEDURAL_OPTIMIZATION on all targets, including the
# vendored librw subproject, so RE3_LTO actually covers the renderer.
if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()

set(EXECUTABLE re3)
set(PROJECT RE3)

project(${EXECUTABLE} C CXX)
set(${PROJECT}_AUTHOR "${PROJECT} Team")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GetGitRevisionDescription)
get_git_head_revision(GIT_REFSPEC GIT_SHA1 "ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR")
message(STATUS "Building ${CMAKE_PROJECT_NAME} GIT SHA1: ${GIT_SHA1}")

if(NINTENDO_SWITCH)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/nx")
    include(NXFunctions)
endif()

if(NOT COMMAND re3_platform_target)
    function(re3_platform_target)
    endfunction()
endif()

if(WIN32)
    set(${PROJECT}_AUDIOS "OAL" "MSS")
else()
    set(${PROJECT}_AUDIOS "OAL")
endif()

set(${PROJECT}_AUDIO "OAL" CACHE STRING "Audio")

option(${PROJECT}_INSTALL "Enable installation of ${EXECUTABLE} + gamefiles" OFF)
option(${PROJECT}_WITH_OPUS "Build ${EXECUTABLE} with opus support" OFF)
option(${PROJECT}_WITH_LIBSNDFILE "Build ${EXECUTABLE} with libsndfile (instead of internal decoder)" OFF)
# Raspberry Pi (VideoCore IV) target: prefer hardware GLES2 context over software desktop GL.
# Without this, XWayland/llvmpipe hands librw a software GL 2.1 context first.
option(${PROJECT}_RPI "Optimize for Raspberry Pi (VideoCore IV / GLES2)" OFF)

# --- Performance build options. Default ON for the Pi (RE3_RPI) so the
# Release cross build ships optimized; OFF elsewhere so desktop/CI are
# unchanged. Only take effect in non-Debug builds. See docs/14. ---
#
# Link-time optimization: lets GCC inline and prune across translation units
# and the vendored static librw (the main win here). Routes ar/ranlib/nm
# through the gcc- wrappers so LTO sections in static archives survive (works
# with the arm cross toolchain too).
option(${PROJECT}_LTO "Enable link-time optimization (-flto=auto) for non-Debug builds" ${${PROJECT}_RPI})

# Extra speed-oriented flags borrowed from the lv_gba_emu Pi build, filtered to
# ones that help runtime speed (NOT the size-first ones like -falign-*=1 /
# -fno-unroll-loops / -fwhole-program, which would hurt speed or break linking
# against librw/SDL/OpenAL):
#   -fno-math-errno                 : don't set errno from libm calls
#   -ffunction-sections/-fdata-sections + --gc-sections : drop dead code, helps I-cache
#   -fno-stack-protector -fno-ident : shave a little overhead
option(${PROJECT}_EXTRA_OPT "Add speed-oriented compile flags for non-Debug builds" ${${PROJECT}_RPI})

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(${PROJECT}_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
        if(_ipo_ok)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
            # For GCC, route ar/ranlib/nm through the -gcc- wrappers so LTO
            # sections in static libraries (e.g. vendored librw.a) survive.
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                get_filename_component(_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
                get_filename_component(_cc_name "${CMAKE_CXX_COMPILER}" NAME)
                string(REPLACE "g++" "gcc" _cc_prefix "${_cc_name}")
                foreach(_tool ar ranlib nm)
                    string(TOUPPER ${_tool} _TOOL)
                    find_program(_gcc_${_tool} "${_cc_prefix}-${_tool}" HINTS "${_cc_dir}")
                    if(_gcc_${_tool})
                        set(CMAKE_${_TOOL} "${_gcc_${_tool}}" CACHE FILEPATH "" FORCE)
                    endif()
                endforeach()
            endif()
            message(STATUS "${PROJECT}_LTO enabled: -flto=auto (IPO)")
        else()
            message(WARNING "${PROJECT}_LTO requested but IPO unsupported: ${_ipo_msg}")
        endif()
    endif()

    if(${PROJECT}_EXTRA_OPT)
        add_compile_options(-fno-math-errno -ffunction-sections -fdata-sections
                            -fno-stack-protector -fno-ident)
        add_link_options(-Wl,--gc-sections)
        message(STATUS "${PROJECT}_EXTRA_OPT enabled: speed-oriented flags + --gc-sections")
    endif()
endif()

set_property(CACHE ${PROJECT}_AUDIO PROPERTY STRINGS ${${PROJECT}_AUDIOS})
message(STATUS "${PROJECT}_AUDIO = ${${PROJECT}_AUDIO} (choices=${${PROJECT}_AUDIOS})")
set("${PROJECT}_AUDIO_${${PROJECT}_AUDIO}" ON)
if(NOT ${PROJECT}_AUDIO IN_LIST ${PROJECT}_AUDIOS)
    message(FATAL_ERROR "Illegal ${PROJECT}_AUDIO=${${PROJECT}_AUDIO}")
endif()

option(${PROJECT}_VENDORED_LIBRW "Use vendored librw" ON)
if(${PROJECT}_RPI)
    # Applies to librw (gl3device.cpp) and re3 alike: prefer hardware GLES2 context.
    add_compile_definitions(RPI_PREFER_GLES)
    message(STATUS "${PROJECT}_RPI enabled: preferring hardware GLES2 context")
endif()

# On-screen FPS + per-stage timing (see docs/03). Debug/optimization aid.
option(${PROJECT}_TIMEBARS "Show on-screen FPS and per-stage frame timers" OFF)
if(${PROJECT}_TIMEBARS)
    add_compile_definitions(TIMEBARS)
    message(STATUS "${PROJECT}_TIMEBARS enabled: on-screen FPS + timers")
endif()

# Also print per-stage timings to stdout/log (read bottlenecks without the screen).
option(${PROJECT}_TIMEBARS_LOG "Print per-stage frame timers to stdout/log" OFF)
if(${PROJECT}_TIMEBARS_LOG)
    add_compile_definitions(TIMEBARS_LOG)
    message(STATUS "${PROJECT}_TIMEBARS_LOG enabled: per-stage timers to log")
endif()

# Drop bandwidth-heavy post-processing (screen rain streaks/droplets + the
# fullscreen-copy colour-filter/motion-blur pass) for low-end GPUs. These are
# cosmetic and cost a lot of fill-rate/bandwidth on VC4. Defaults to ON on the
# Pi (RE3_RPI) so a plain Pi build never silently re-enables them; still
# overridable with -DRE3_RPI_LEAN_FX=OFF for A/B testing.
option(${PROJECT}_RPI_LEAN_FX "Disable fullscreen-copy post effects (screen rain droplets)" ${${PROJECT}_RPI})
if(${PROJECT}_RPI_LEAN_FX)
    add_compile_definitions(RPI_LEAN_FX)
    message(STATUS "${PROJECT}_RPI_LEAN_FX enabled: lean post-processing (no screen droplets)")
endif()

# Output backend for the GBM (headless) skeleton: push frames to an ST7789 SPI
# panel (drivers/st7789) instead of /dev/fb0. Only meaningful with the GBM
# gfxlib. See docs/06, docs/08.
option(${PROJECT}_OUTPUT_SPI "GBM skeleton pushes frames to an ST7789 SPI panel instead of /dev/fb0" OFF)
if(${PROJECT}_OUTPUT_SPI)
    add_compile_definitions(RE3_OUTPUT_SPI)
    message(STATUS "${PROJECT}_OUTPUT_SPI enabled: ST7789 SPI output (drivers/st7789)")
endif()

# Input source for the GBM (headless) skeleton: read keyboard, mouse and
# gamepads via evdev (/dev/input/event*) with hotplug, feeding re3's keyboard
# event chain and pad state. Lets the headless build take input without
# GLFW/X11. Defaults ON for the Pi so a plain Pi build has working input.
# See docs/08.
option(${PROJECT}_INPUT_EVDEV "GBM skeleton reads keyboard/mouse/gamepad via evdev (/dev/input)" ${${PROJECT}_RPI})
if(${PROJECT}_INPUT_EVDEV)
    add_compile_definitions(RE3_INPUT_EVDEV)
    message(STATUS "${PROJECT}_INPUT_EVDEV enabled: evdev keyboard/mouse/gamepad input")
endif()

# Simplified Chinese localization. Reuses the game's Japanese CJK font pipeline
# (big glyph atlas + full-width layout) but widens the atlas geometry to hold
# ~1600 Chinese glyphs (64 cols x 32 rows vs Japanese 48 x 25.6). Ship the
# Chinese assets in the Japanese slots (JAPANESE.GXT + MODELS/FONTS_J.TXD) and
# select the Japanese language; those are produced offline by tools/gen_cn_font
# + tools/png2txd (see docs/11). Requires MORE_LANGUAGES (default on).
option(${PROJECT}_CHINESE "Simplified Chinese via the CJK font pipeline (widened atlas)" OFF)
if(${PROJECT}_CHINESE)
    add_compile_definitions(RE3_CHINESE)
    message(STATUS "${PROJECT}_CHINESE enabled: Simplified Chinese localization")
endif()

# Desktop/emulator debug backends for the GBM skeleton: present the readback in
# an SDL2 window and/or take keyboard input from SDL2. Lets the exact GBM render
# path (EGL+GBM surfaceless GLES2) run and be debugged on a desktop. See docs/08.
option(${PROJECT}_OUTPUT_SDL "GBM skeleton presents frames in an SDL2 window (desktop debug)" OFF)
if(${PROJECT}_OUTPUT_SDL)
    add_compile_definitions(RE3_OUTPUT_SDL)
    message(STATUS "${PROJECT}_OUTPUT_SDL enabled: SDL2 window output")
endif()
option(${PROJECT}_INPUT_SDL "GBM skeleton reads keyboard input from SDL2 (desktop debug)" OFF)
if(${PROJECT}_INPUT_SDL)
    add_compile_definitions(RE3_INPUT_SDL)
    message(STATUS "${PROJECT}_INPUT_SDL enabled: SDL2 keyboard input")
endif()

if(${PROJECT}_VENDORED_LIBRW)
    add_subdirectory(vendor/librw)
else()
    find_package(librw REQUIRED)
endif()
add_subdirectory(src)

if(${PROJECT}_INSTALL)
    install(DIRECTORY gamefiles/ DESTINATION ".")
    if(LIBRW_PLATFORM_NULL)
        set(platform "-null")
    elseif(LIBRW_PLATFORM_PS2)
        set(platform "-ps2")
    elseif(LIBRW_PLATFORM_GL3)
        if(LIBRW_GL3_GFXLIB STREQUAL "GLFW")
            set(platform "-gl3-glfw")
        else()
            set(platform "-gl3-sdl2")
        endif()
    elseif(LIBRW_PLATFORM_D3D9)
        set(platform "-d3d9")
    endif()
    if(${PROJECT}_AUDIO_OAL)
        set(audio "-oal")
    elseif(${PROJECT}_AUDIO_MSS)
        set(audio "-mss")
    endif()
    if(${PROJECT}_WITH_OPUS)
        set(audio "${audio}-opus")
    endif()
    if(NOT LIBRW_PLATFORM_PS2)
        if(WIN32)
            set(os "-win")
        elseif(APPLE)
            set(os "-apple")
        elseif(UNIX)
            set(os "-linux")
        elseif(NINTENDO_SWITCH)
            set(os "-switch")
        else()
            set(compiler "-UNK")
            message(WARNING "Unknown os. Created cpack package will be wrong. (override using cpack -P)")
        endif()
    endif()

    set(CPACK_PACKAGE_NAME "${PROJECT_NAME}${platform}${audio}${os}${compiler}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "GTA III reversed")
    set(CPACK_PACKAGE_VENDOR "GTAModding")
    # FIXME: missing license (https://github.com/GTAmodding/re3/issues/794)
    # set(CPACK_PACKAGE_DESCRIPTION_FILE "${PROJECT_SOURCE_DIR}/LICENSE")
    # set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_NAME}")
    set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}")
    set(CPACK_GENERATOR "ZIP")
    include(CPack)
endif()
