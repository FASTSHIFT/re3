# ST7789 SPI display driver (Raspberry Pi)
#
# Produces a static library `st7789` that upper layers (e.g. re3) can link.
# Also builds an example/smoke-test when ST7789_BUILD_EXAMPLE is ON.

cmake_minimum_required(VERSION 3.10)
project(st7789 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(st7789 STATIC
    st7789.c
    pi_gpio.c
)

target_include_directories(st7789 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(st7789 PRIVATE -O2 -Wall -Wextra)
endif()

option(ST7789_BUILD_EXAMPLE "Build the ST7789 example/smoke test" ON)
if(ST7789_BUILD_EXAMPLE)
    add_executable(st7789_example example.c)
    target_link_libraries(st7789_example PRIVATE st7789)

    # Diagnostic pattern tool for finding the safe SPI clock (continuous refresh).
    add_executable(st7789_pattern pattern.c)
    target_link_libraries(st7789_pattern PRIVATE st7789)
endif()
