# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.13)

# This project intentionally supports the original RP2040 Pico only. FORCE
# prevents a cached Pico 2 selection from silently changing the target.
set(PICO_BOARD "pico" CACHE STRING "Target Raspberry Pi Pico board" FORCE)
set(PICO_PLATFORM "rp2040" CACHE STRING "Target Raspberry Pi platform" FORCE)

# Prefer an explicit SDK location, then the standard environment variable. The
# /opt fallback is convenient for the repository's current development setup.
if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if(NOT PICO_SDK_PATH AND EXISTS "/opt/pico-sdk/external/pico_sdk_import.cmake")
    set(PICO_SDK_PATH "/opt/pico-sdk")
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR
        "Set PICO_SDK_PATH to a Raspberry Pi Pico SDK installation.")
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

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
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/pico_extras_import.cmake")

project(p2000t_vid2vga_firmware VERSION 0.9.1 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

option(P2000T_PROTOTYPE_V1_MIRRORED_DIN
    "Use the mirrored DIN wiring of the assembled prototype-v1 PCB/cable"
    ON)

pico_sdk_init()

add_subdirectory(src)
