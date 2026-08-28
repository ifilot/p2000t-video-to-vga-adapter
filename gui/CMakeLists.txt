# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)

project(p2000t_vid2vga_capture VERSION 0.4.1 LANGUAGES CXX)

file(WRITE "${CMAKE_BINARY_DIR}/package-version.txt" "${PROJECT_VERSION}\n")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

set(P2000T_HOST_SOURCES
    main.cpp
    adapter_serial_port.cpp
    adapter_serial_port.h
    codex_lab_bridge.cpp
    codex_lab_bridge.h
    configuration_dialog.cpp
    configuration_dialog.h
    engineering_autotune.cpp
    engineering_autotune.h
    no_signal_screen.cpp
    no_signal_screen.h
    pico_configuration.cpp
    pico_configuration.h
    resources.qrc
)

qt_add_executable(p2000t-vid2vga-capture ${P2000T_HOST_SOURCES})
qt_add_executable(p2000t-vid2vga-lab-agent ${P2000T_HOST_SOURCES})

foreach(host_target p2000t-vid2vga-capture p2000t-vid2vga-lab-agent)
    target_include_directories(${host_target} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../src)
    target_compile_definitions(${host_target} PRIVATE
        P2000T_VIEWER_VERSION="${PROJECT_VERSION}")
    target_link_libraries(${host_target} PRIVATE Qt6::Widgets)
endforeach()
target_compile_definitions(p2000t-vid2vga-lab-agent PRIVATE
    P2000T_LAB_AGENT_ONLY=1)

if(MSVC)
    target_compile_options(p2000t-vid2vga-capture PRIVATE /W4)
    target_compile_options(p2000t-vid2vga-lab-agent PRIVATE /W4)
else()
    target_compile_options(p2000t-vid2vga-capture PRIVATE
        -Wall -Wextra -Wpedantic)
    target_compile_options(p2000t-vid2vga-lab-agent PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_sources(p2000t-vid2vga-capture PRIVATE p2000t-capture.rc)
    target_sources(p2000t-vid2vga-lab-agent PRIVATE p2000t-capture.rc)
    target_compile_definitions(p2000t-vid2vga-capture PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_definitions(p2000t-vid2vga-lab-agent PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(p2000t-vid2vga-capture PRIVATE setupapi)
    target_link_libraries(p2000t-vid2vga-lab-agent PRIVATE setupapi)
endif()

set_target_properties(p2000t-vid2vga-capture PROPERTIES
    WIN32_EXECUTABLE TRUE
)
set_target_properties(p2000t-vid2vga-lab-agent PROPERTIES
    WIN32_EXECUTABLE TRUE
)

install(TARGETS p2000t-vid2vga-capture p2000t-vid2vga-lab-agent
    RUNTIME DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    add_executable(p2000t-packbits-test ../tests/packbits_test.cpp)
    target_include_directories(p2000t-packbits-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../src)
    if(NOT MSVC)
        target_compile_options(p2000t-packbits-test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME packbits-round-trip COMMAND p2000t-packbits-test)

    add_executable(p2000t-reconstruction-test
        ../tests/reconstruction_test.cpp)
    target_include_directories(p2000t-reconstruction-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../src)
    if(NOT MSVC)
        target_compile_options(p2000t-reconstruction-test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME guarded-second-tap COMMAND p2000t-reconstruction-test)

    add_executable(p2000t-configuration-protocol-test
        ../tests/configuration_protocol_test.cpp
        pico_configuration.cpp
        pico_configuration.h
    )
    target_include_directories(p2000t-configuration-protocol-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../src)
    target_link_libraries(p2000t-configuration-protocol-test PRIVATE Qt6::Core)
    if(NOT MSVC)
        target_compile_options(p2000t-configuration-protocol-test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME configuration-protocol
        COMMAND p2000t-configuration-protocol-test)

    add_executable(p2000t-engineering-autotune-test
        ../tests/engineering_autotune_test.cpp
        engineering_autotune.cpp
        engineering_autotune.h
    )
    target_include_directories(p2000t-engineering-autotune-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(p2000t-engineering-autotune-test PRIVATE Qt6::Gui)
    if(NOT MSVC)
        target_compile_options(p2000t-engineering-autotune-test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME engineering-autotune-scoring
        COMMAND p2000t-engineering-autotune-test)

    add_executable(p2000t-codex-lab-bridge-test
        ../tests/codex_lab_bridge_test.cpp
        codex_lab_bridge.cpp
        codex_lab_bridge.h
    )
    target_include_directories(p2000t-codex-lab-bridge-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../src)
    target_link_libraries(p2000t-codex-lab-bridge-test PRIVATE Qt6::Core)
    if(NOT MSVC)
        target_compile_options(p2000t-codex-lab-bridge-test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME codex-lab-request-protocol
        COMMAND p2000t-codex-lab-bridge-test)

    qt_add_executable(p2000t-no-signal-screen-test
        ../tests/no_signal_screen_test.cpp
        no_signal_screen.cpp
        no_signal_screen.h
        resources.qrc
    )
    target_include_directories(p2000t-no-signal-screen-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../src)
    target_compile_definitions(p2000t-no-signal-screen-test PRIVATE
        P2000T_VIEWER_VERSION="${PROJECT_VERSION}")
    target_link_libraries(p2000t-no-signal-screen-test PRIVATE Qt6::Widgets)
    if(NOT MSVC)
        target_compile_options(p2000t-no-signal-screen-test PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME no-signal-screen-composition
        COMMAND p2000t-no-signal-screen-test)
endif()
