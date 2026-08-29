# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.13)

# Prefer project compatibility modules before the SDK's fallback modules.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Default to the original Pico while allowing a separate build directory to
# select Pico 2 with -DPICO_BOARD=pico2. The SDK derives PICO_PLATFORM from the
# selected board.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico" CACHE STRING "Target Raspberry Pi Pico board")
endif()

# Prefer an explicit SDK location. The /opt fallback is convenient for local
# installations; otherwise the pinned SDK release is downloaded automatically.
if(NOT PICO_SDK_PATH AND EXISTS "/opt/pico-sdk/external/pico_sdk_import.cmake")
    set(PICO_SDK_PATH "/opt/pico-sdk")
endif()
set(PICO_SDK_FETCH_FROM_GIT ON CACHE BOOL
    "Automatically download the Raspberry Pi Pico SDK")
set(PICO_SDK_FETCH_FROM_GIT_TAG "2.1.1" CACHE STRING
    "Pico SDK release used by the firmware")
set(PICO_SDK_FETCH_FROM_GIT_PATH
    "${CMAKE_CURRENT_LIST_DIR}/.ci-dependencies" CACHE PATH
    "Shared download directory for the Pico SDK")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake")

# VGA generation uses pico-extras' scanvideo implementation. Prefer an
# installed checkout, but allow CMake to fetch the SDK-matched release for a
# clean build.
if(NOT PICO_EXTRAS_PATH AND DEFINED ENV{PICO_EXTRAS_PATH})
    set(PICO_EXTRAS_PATH "$ENV{PICO_EXTRAS_PATH}")
endif()
if(NOT PICO_EXTRAS_PATH)
    set(PICO_EXTRAS_FETCH_FROM_GIT ON CACHE BOOL
        "Automatically download Raspberry Pi pico-extras")
    set(PICO_EXTRAS_FETCH_FROM_GIT_TAG "sdk-2.1.1" CACHE STRING
        "pico-extras release used by the firmware")
    set(PICO_EXTRAS_FETCH_FROM_GIT_PATH
        "${CMAKE_CURRENT_LIST_DIR}/.ci-dependencies" CACHE PATH
        "Shared download directory for pico-extras")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/pico_extras_import.cmake")

project(p2000t_vid2vga_firmware VERSION 0.4.1 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

option(P2000T_PROTOTYPE_V1_MIRRORED_DIN
    "Use the mirrored DIN wiring of the assembled prototype-v1 PCB/cable"
    ON)

option(P2000T_WINDOW_CAPTURE_ENABLED
    "Enable the RP2350 line-sliced six-tap capture engine"
    ON)

pico_sdk_init()

add_subdirectory(src)
