# Find or build the Pico SDK's pioasm host utility.
#
# This local copy adds an explicit <cstdint> include for SDK 2.1.1, whose
# pioasm sources otherwise fail with recent GCC host compilers.
#
# SPDX-FileCopyrightText: 2020 Raspberry Pi (Trading) Ltd.
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: BSD-3-Clause

if(NOT TARGET pioasm)
    include(ExternalProject)

    set(PIOASM_SOURCE_DIR "${PICO_SDK_PATH}/tools/pioasm")
    set(PIOASM_BINARY_DIR "${CMAKE_BINARY_DIR}/pioasm")
    set(PIOASM_INSTALL_DIR "${CMAKE_BINARY_DIR}/pioasm-install" CACHE PATH
        "Directory where pioasm is installed" FORCE)

    set(_pioasm_cmake_args
        "--no-warn-unused-cli"
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
        "-DPIOASM_FLAT_INSTALL=1"
        "-DCMAKE_INSTALL_PREFIX=${PIOASM_INSTALL_DIR}"
        "-DCMAKE_RULE_MESSAGES=OFF"
        "-DCMAKE_INSTALL_MESSAGE=NEVER"
    )
    if(CMAKE_HOST_UNIX)
        list(APPEND _pioasm_cmake_args
            "-DCMAKE_CXX_FLAGS:STRING=-include cstdint")
    endif()

    if(NOT TARGET pioasmBuild)
        ExternalProject_Add(pioasmBuild
            PREFIX pioasm
            SOURCE_DIR "${PIOASM_SOURCE_DIR}"
            BINARY_DIR "${PIOASM_BINARY_DIR}"
            INSTALL_DIR "${PIOASM_INSTALL_DIR}"
            CMAKE_ARGS ${_pioasm_cmake_args}
            CMAKE_CACHE_ARGS
                "-DPIOASM_EXTRA_SOURCE_FILES:STRING=${PIOASM_EXTRA_SOURCE_FILES}"
            BUILD_ALWAYS 1
            EXCLUDE_FROM_ALL TRUE
        )
    endif()

    if(CMAKE_HOST_WIN32)
        set(pioasm_EXECUTABLE "${PIOASM_INSTALL_DIR}/pioasm/pioasm.exe")
    else()
        set(pioasm_EXECUTABLE "${PIOASM_INSTALL_DIR}/pioasm/pioasm")
    endif()
    add_executable(pioasm IMPORTED GLOBAL)
    set_property(TARGET pioasm PROPERTY IMPORTED_LOCATION
        "${pioasm_EXECUTABLE}")
    add_dependencies(pioasm pioasmBuild)

    unset(_pioasm_cmake_args)
endif()
