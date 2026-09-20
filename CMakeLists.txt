cmake_minimum_required(VERSION 3.13)

# PicoBench - Pico Hardware Characterization & Benchmark.
# Build one platform per build directory:
#   cmake -S . -B build/rp2040       -G Ninja -DPICO_PLATFORM=rp2040
#   cmake -S . -B build/rp2350-arm-s -G Ninja -DPICO_PLATFORM=rp2350-arm-s
#   cmake -S . -B build/rp2350-riscv -G Ninja -DPICO_PLATFORM=rp2350-riscv
# build.sh does all of this.

set(PICO_BOARD picobench CACHE STRING "Board type")
# Linker FLASH region = firmware image limit (1 MB). The real flash is detected at
# runtime; the results store and scratch area live at its end (src/storage/).
set(PICO_FLASH_SIZE_BYTES "1024 * 1024")
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
option(PICOBENCH_TEST_CRASH "Include the debug.crash test that hangs on purpose" OFF)

add_executable(picobench
    src/main.c
    src/core/bench.c
    src/core/button.c
    src/core/clocks_ctl.c
    src/core/guard.c
    src/core/led.c
    src/core/platform.c
    src/core/sequencer.c
    src/core/stats.c
    src/core/timing.c
    src/results/boot_status.c
    src/results/crc32.c
    src/results/record.c
    src/results/report_csv.c
    src/results/report_errors.c
    src/results/report_hardware.c
    src/results/report_text.c
    src/results/sink.c
    src/results/summary.c
    src/detect/board.c
    src/detect/flash_info.c
    src/detect/inventory.c
    src/detect/psram_detect.c
    src/storage/flash_layout.c
    src/storage/results_store.c
    src/tests/registry.c
    src/tests/selftest.c
    src/usb/console.c
    src/usb/msc_disk.c
    src/usb/usb_descriptors.c
)

target_compile_definitions(picobench PRIVATE
    $<$<BOOL:${PICOBENCH_TEST_CRASH}>:PICOBENCH_TEST_CRASH=1>
)

target_include_directories(picobench PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/usb          # tusb_config.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_compile_options(picobench PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Boards with RP2350-A2 silicon exist in the field (seen on a Pico 2 clone), so keep the
# SDK's A2 errata workarounds (notably E14 in the PSRAM setup) enabled.
target_compile_definitions(picobench PRIVATE PICO_RP2350_A2_SUPPORTED=1)

target_link_libraries(picobench
    pico_stdlib
    pico_rand
    hardware_pwm
    hardware_adc
    hardware_clocks
    hardware_flash
    hardware_pll
    pico_unique_id
    tinyusb_device
)

# No stdio transport at all. USB (report disk + console, own TinyUSB descriptors) is
# started by main() only after the run has finished and the report is committed.
if (PICO_PLATFORM MATCHES "rp2350")
    target_link_libraries(picobench hardware_psram hardware_xip_cache)
endif()

pico_enable_stdio_usb(picobench 0)
pico_enable_stdio_uart(picobench 0)

pico_set_program_name(picobench "PicoBench")
pico_set_program_description(picobench "Pico Hardware Characterization & Benchmark")
pico_set_program_version(picobench "${PICOBENCH_VERSION}+${PICOBENCH_GIT_SHA}")

pico_add_extra_outputs(picobench)
