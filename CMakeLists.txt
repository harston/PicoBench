cmake_minimum_required(VERSION 3.13)

# PicoBench - Pico Hardware Characterization & Benchmark.
# Build one platform per build directory:
#   cmake -S . -B build/rp2040       -G Ninja -DPICO_PLATFORM=rp2040
#   cmake -S . -B build/rp2350-arm-s -G Ninja -DPICO_PLATFORM=rp2350-arm-s
#   cmake -S . -B build/rp2350-riscv -G Ninja -DPICO_PLATFORM=rp2350-riscv
# build.sh does all of this.

set(PICO_BOARD picobench CACHE STRING "Board type")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include(cmake/pico_sdk_import.cmake)

project(picobench C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.3.0")
    message(FATAL_ERROR "PicoBench requires Pico SDK 2.3.0 or later (found ${PICO_SDK_VERSION_STRING})")
endif()

pico_sdk_init()

# ---- Version / build identification -------------------------------------------
# Regenerated on every configure; build.sh always reconfigures before building.
set(PICOBENCH_VERSION "0.0.1")
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PICOBENCH_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT PICOBENCH_GIT_SHA)
    set(PICOBENCH_GIT_SHA "nogit")
endif()
execute_process(
    COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PICOBENCH_GIT_DIRTY_OUT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (PICOBENCH_GIT_DIRTY_OUT)
    set(PICOBENCH_GIT_DIRTY 1)
else()
    set(PICOBENCH_GIT_DIRTY 0)
endif()
string(TIMESTAMP PICOBENCH_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
# Build ID: changes with every build, used to decide "new firmware -> run tests once".
string(SHA256 PICOBENCH_BUILD_HASH "${PICOBENCH_VERSION}|${PICOBENCH_GIT_SHA}|${PICOBENCH_GIT_DIRTY}|${PICOBENCH_BUILD_TIME}|${PICO_PLATFORM}")
string(SUBSTRING "${PICOBENCH_BUILD_HASH}" 0 8 PICOBENCH_BUILD_ID)
configure_file(src/core/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/picobench_version.h @ONLY)

# ---- Firmware -----------------------------------------------------------------
add_executable(picobench
    src/main.c
    src/core/led.c
)

target_include_directories(picobench PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_compile_options(picobench PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(picobench
    pico_stdlib
    hardware_pwm
    hardware_clocks
)

# No stdio transport during measurements; USB is brought up only for the report disk.
pico_enable_stdio_usb(picobench 0)
pico_enable_stdio_uart(picobench 0)

pico_set_program_name(picobench "PicoBench")
pico_set_program_description(picobench "Pico Hardware Characterization & Benchmark")
pico_set_program_version(picobench "${PICOBENCH_VERSION}+${PICOBENCH_GIT_SHA}")

pico_add_extra_outputs(picobench)
