find_package(Threads REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)

file(GLOB_RECURSE ${PROJECT}_SOURCES "*.cpp" "*.h" "*.rc")

function(header_directories RETURN_LIST)
    file(GLOB_RECURSE ALL_SRCS *.h *.cpp *.c)
    set(RELDIRS)
    foreach(SRC ${ALL_SRCS})
        file(RELATIVE_PATH RELSRC "${CMAKE_CURRENT_SOURCE_DIR}" "${SRC}")
        get_filename_component(RELDIR "${RELSRC}" DIRECTORY)
        list(APPEND RELDIRS ${RELDIR})
    endforeach()
    list(REMOVE_DUPLICATES RELDIRS)
    set(${RETURN_LIST} ${RELDIRS} PARENT_SCOPE)
endfunction()

header_directories(${PROJECT}_INCLUDES)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp" @ONLY)
list(APPEND ${PROJECT}_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp")

add_executable(${EXECUTABLE} WIN32
    ${${PROJECT}_SOURCES}
)

target_link_libraries(${EXECUTABLE} PRIVATE
    librw::librw
    Threads::Threads
)

target_include_directories(${EXECUTABLE}
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${${PROJECT}_INCLUDES}>
)

target_compile_definitions(${EXECUTABLE}
    PRIVATE
        $<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>
        LIBRW
        CMAKE_NO_AUTOLINK
)

if(LIBRW_PLATFORM_D3D9)
    target_compile_definitions(${EXECUTABLE}
        PUBLIC
            USE_D3D9
    )
endif()

target_compile_definitions(${EXECUTABLE} PRIVATE CMAKE_BUILD)
target_compile_definitions(${EXECUTABLE} PRIVATE USE_OUR_VERSIONING)

if(${PROJECT}_AUDIO STREQUAL "OAL")
    find_package(OpenAL REQUIRED)
    if(TARGET OpenAL::OpenAL)
        target_link_libraries(${EXECUTABLE} PRIVATE OpenAL::OpenAL)
    else()
        target_include_directories(${EXECUTABLE} PRIVATE ${OPENAL_INCLUDE_DIR})
        target_link_libraries(${EXECUTABLE} PRIVATE ${OPENAL_LIBRARY})
        target_compile_definitions(${EXECUTABLE} PRIVATE ${OPENAL_DEFINITIONS})
    endif()
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL)
elseif(${PROJECT}_AUDIO STREQUAL "MSS")
    find_package(MilesSDK REQUIRED)
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_MSS)
    target_link_libraries(${EXECUTABLE} PRIVATE MilesSDK::MilesSDK)
endif()

find_package(mpg123 REQUIRED)
target_link_libraries(${EXECUTABLE} PRIVATE
    MPG123::libmpg123
)
if(${PROJECT}_WITH_OPUS)
    find_package(opusfile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        opusfile::opusfile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OPUS)
endif()
if(${PROJECT}_WITH_LIBSNDFILE)
    find_package(SndFile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE
        SndFile::SndFile
    )
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL_USE_SNDFILE)
endif()

target_compile_definitions(${EXECUTABLE} PRIVATE )

option(${PROJECT}_WITH_SANITIZERS "Use UB sanitizers (better crash log)" OFF)
option(${PROJECT}_WITH_ASAN "Use Address sanitizer (better crash log)" OFF)

if(${PROJECT}_WITH_SANITIZERS)
    target_compile_options(${EXECUTABLE} PUBLIC
            -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability
            -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,implicit-integer-truncation,implicit-integer-arithmetic-value-change,local-bounds,nullability)
endif()

if(${PROJECT}_WITH_ASAN)
    target_compile_options(${EXECUTABLE} PUBLIC -fsanitize=address -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC -fsanitize=address)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(${EXECUTABLE}
        PRIVATE
            "-Wall"
    )
    if (NOT LIBRW_PLATFORM_PS2)
        target_compile_options(${EXECUTABLE}
            PRIVATE
                -Wextra
                -Wdouble-promotion
                -Wpedantic
        )
    endif()
    # When OFF (e.g. CI on a modern GCC), silence the large set of pre-existing
    # upstream re3 warnings that only surface on newer compilers. Appended
    # *after* -Wall/-Wextra/-Wpedantic so they take effect. Newly written code
    # should still be warning-clean under the strict set (turn this ON locally).
    option(${PROJECT}_STRICT_WARNINGS "Treat all warnings strictly (no upstream suppressions)" ON)
    if(NOT ${PROJECT}_STRICT_WARNINGS)
        # Language-agnostic suppressions (valid for both C and C++).
        target_compile_options(${EXECUTABLE}
            PRIVATE
                -Wno-pedantic
                -Wno-unused-parameter
                -Wno-sign-compare
                -Wno-double-promotion
                -Wno-missing-field-initializers
                -Wno-strict-aliasing
                -Wno-maybe-uninitialized
                -Wno-implicit-fallthrough
                -Wno-parentheses
                -Wno-unused-variable
                -Wno-unused-but-set-variable
                -Wno-ignored-qualifiers
                -Wno-unknown-pragmas
                -Wno-stringop-truncation
                -Wno-stringop-overflow
                -Wno-format-truncation
                -Wno-array-bounds
                -Wno-unused-result
                -Wno-int-to-pointer-cast
                -Wno-misleading-indentation
                -Wno-format-overflow
                -Wno-multichar
                -Wno-type-limits
                -Wno-restrict
                -Wno-format
                -Wno-cast-function-type
                -Wno-unused-value
                -Wno-unused-function
        )
        # C++-only suppressions (these flags warn "not valid for C" otherwise).
        target_compile_options(${EXECUTABLE}
            PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-copy>
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder>
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-class-memaccess>
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-offsetof>
        )
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${EXECUTABLE}
        PUBLIC
            /Zc:sizedDealloc-
    )
endif()

if(NINTENDO_SWITCH)
    set(${PROJECT}_C_CXX_EXTENSIONS ON)
else()
    set(${PROJECT}_C_CXX_EXTENSIONS OFF)
endif()

if(LIBRW_PLATFORM_GL3 AND LIBRW_GL3_GFXLIB STREQUAL "GLFW")
	include(CheckSymbolExists)

	set(CMAKE_REQUIRED_LIBRARIES glfw)
	set(CMAKE_REQUIRED_DEFINITIONS -DGLFW_EXPOSE_NATIVE_X11)
	check_symbol_exists(glfwGetX11Display "GLFW/glfw3.h;GLFW/glfw3native.h" GLFW_HAS_X11)
	unset(CMAKE_REQUIRED_DEFINITIONS)
	unset(CMAKE_REQUIRED_LIBRARIES)

	if (GLFW_HAS_X11)
		find_package(X11 REQUIRED)
		target_link_libraries(${EXECUTABLE} PRIVATE X11::X11)
		target_compile_definitions(${EXECUTABLE} PRIVATE GET_KEYBOARD_INPUT_FROM_X11)
	endif (GLFW_HAS_X11)
endif()

# GBM/EGL surfaceless backend (no window system): src/skel/gbm/gbm.cpp reads
# back the framebuffer with glReadPixels and presents to /dev/fb0 or SPI.
# We only need GLESv2 for the readback call; librw already propagates gbm + EGL
# as PUBLIC link deps.
# NOTE: gate on the CACHE variable LIBRW_GL3_GFXLIB (visible here). The
# LIBRW_PLATFORM_GL3 marker is a plain variable set in librw's own directory
# scope and is NOT visible in this subdirectory, so we must not rely on it.
if(LIBRW_GL3_GFXLIB STREQUAL "GBM")
	target_link_libraries(${EXECUTABLE} PRIVATE GLESv2 EGL gbm)

	# The GBM skeleton's pluggable backends live in src/skel/gbm/{sink,input}_*.cpp
	# and are picked up by the GLOB_RECURSE above; each file self-guards on the
	# RE3_OUTPUT_* / RE3_INPUT_* macros so exactly one sink and one source
	# compile per build.

	# SPI output backend: compile the ST7789 driver into re3 (GLOB only scans
	# src/, so add the driver sources explicitly) so sink_spi/input_gpio can use it.
	if(RE3_OUTPUT_SPI)
		target_sources(${EXECUTABLE} PRIVATE
			${PROJECT_SOURCE_DIR}/drivers/st7789/st7789.c
			${PROJECT_SOURCE_DIR}/drivers/st7789/pi_gpio.c
		)
		target_include_directories(${EXECUTABLE} PRIVATE
			${PROJECT_SOURCE_DIR}/drivers/st7789
		)
	endif()

	# Our own new code (GBM skeleton + ST7789 driver) must stay warning-clean:
	# compile just these files with -Werror so regressions fail the build,
	# while upstream re3 code keeps warnings non-fatal.
	if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		file(GLOB _re3_own_sources ${PROJECT_SOURCE_DIR}/src/skel/gbm/*.cpp)
		set(_re3_own_c_sources
			${PROJECT_SOURCE_DIR}/drivers/st7789/st7789.c
			${PROJECT_SOURCE_DIR}/drivers/st7789/pi_gpio.c)
		set_source_files_properties(
			${_re3_own_sources} ${_re3_own_c_sources}
			PROPERTIES COMPILE_OPTIONS "-Werror"
		)
	endif()

	# SDL debug backend (desktop/emulator): sink_sdl + input_sdl need SDL2.
	if(RE3_OUTPUT_SDL OR RE3_INPUT_SDL)
		find_package(SDL2 REQUIRED)
		target_link_libraries(${EXECUTABLE} PRIVATE SDL2::SDL2)
	endif()
endif()

set_target_properties(${EXECUTABLE}
    PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS ${${PROJECT}_C_CXX_EXTENSIONS}
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 11
        CXX_EXTENSIONS ${${PROJECT}_C_CXX_EXTENSIONS}
        CXX_STANDARD_REQUIRED ON
)

if(${PROJECT}_INSTALL)
    install(
        TARGETS ${EXECUTABLE}
        EXPORT ${EXECUTABLE}-targets
        RUNTIME DESTINATION "."
    )
    if(MSVC)
        install(FILES $<TARGET_PDB_FILE:${EXECUTABLE}> DESTINATION "." OPTIONAL)
    endif()
endif()

re3_platform_target(${EXECUTABLE} INSTALL)
