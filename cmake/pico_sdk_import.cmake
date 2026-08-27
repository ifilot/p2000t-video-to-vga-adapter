# Fetch or import the Raspberry Pi Pico SDK before project() is called.
#
# SPDX-FileCopyrightText: 2020 Raspberry Pi (Trading) Ltd.
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: BSD-3-Clause

if(DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to the Raspberry Pi Pico SDK")
set(PICO_SDK_FETCH_FROM_GIT "${PICO_SDK_FETCH_FROM_GIT}" CACHE BOOL
    "Fetch the Pico SDK from Git when PICO_SDK_PATH is unset")
set(PICO_SDK_FETCH_FROM_GIT_PATH "${PICO_SDK_FETCH_FROM_GIT_PATH}"
    CACHE PATH "Directory in which CMake downloads the Pico SDK")
set(PICO_SDK_FETCH_FROM_GIT_TAG "${PICO_SDK_FETCH_FROM_GIT_TAG}"
    CACHE STRING "Pico SDK Git tag or commit to fetch")

if(NOT PICO_SDK_PATH)
    if(NOT PICO_SDK_FETCH_FROM_GIT)
        message(FATAL_ERROR
            "PICO_SDK_PATH is unset and automatic download is disabled.")
    endif()

    include(FetchContent)
    file(MAKE_DIRECTORY "${PICO_SDK_FETCH_FROM_GIT_PATH}")
    get_filename_component(_pico_sdk_fetch_dir
        "${PICO_SDK_FETCH_FROM_GIT_PATH}" REALPATH
        BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(_pico_sdk_source_dir "${_pico_sdk_fetch_dir}/pico_sdk-src")

    if(NOT EXISTS "${_pico_sdk_source_dir}/pico_sdk_init.cmake")
        message(STATUS
            "Downloading Pico SDK ${PICO_SDK_FETCH_FROM_GIT_TAG}")
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.17")
            FetchContent_Populate(
                pico_sdk
                QUIET
                GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk.git
                GIT_TAG "${PICO_SDK_FETCH_FROM_GIT_TAG}"
                GIT_SHALLOW TRUE
                GIT_SUBMODULES_RECURSE TRUE
                SOURCE_DIR "${_pico_sdk_source_dir}"
                BINARY_DIR "${_pico_sdk_fetch_dir}/pico_sdk-build"
                SUBBUILD_DIR "${_pico_sdk_fetch_dir}/pico_sdk-subbuild"
            )
        else()
            FetchContent_Populate(
                pico_sdk
                QUIET
                GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk.git
                GIT_TAG "${PICO_SDK_FETCH_FROM_GIT_TAG}"
                GIT_SHALLOW TRUE
                SOURCE_DIR "${_pico_sdk_source_dir}"
                BINARY_DIR "${_pico_sdk_fetch_dir}/pico_sdk-build"
                SUBBUILD_DIR "${_pico_sdk_fetch_dir}/pico_sdk-subbuild"
            )
        endif()
    endif()
    set(PICO_SDK_PATH "${_pico_sdk_source_dir}")
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH
    BASE_DIR "${CMAKE_BINARY_DIR}")
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "${PICO_SDK_PATH} does not contain a Pico SDK checkout.")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to the Raspberry Pi Pico SDK" FORCE)
include("${PICO_SDK_PATH}/pico_sdk_init.cmake")

unset(_pico_sdk_fetch_dir)
unset(_pico_sdk_source_dir)
