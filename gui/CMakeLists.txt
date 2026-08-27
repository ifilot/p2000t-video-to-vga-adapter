# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)

project(p2000t_vid2vga_capture VERSION 0.3.0 LANGUAGES CXX)

file(WRITE "${CMAKE_BINARY_DIR}/package-version.txt" "${PROJECT_VERSION}\n")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

qt_add_executable(p2000t-vid2vga-capture
    main.cpp
    adapter_serial_port.cpp
    adapter_serial_port.h
    configuration_dialog.cpp
    configuration_dialog.h
    no_signal_screen.cpp
    no_signal_screen.h
    pico_configuration.cpp
    pico_configuration.h
    resources.qrc
)

target_include_directories(p2000t-vid2vga-capture PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../src
)

target_compile_definitions(p2000t-vid2vga-capture PRIVATE
    P2000T_VIEWER_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(p2000t-vid2vga-capture PRIVATE /W4)
else()
    target_compile_options(p2000t-vid2vga-capture PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(p2000t-vid2vga-capture PRIVATE Qt6::Widgets)

if(WIN32)
    target_sources(p2000t-vid2vga-capture PRIVATE p2000t-capture.rc)
    target_compile_definitions(p2000t-vid2vga-capture PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(p2000t-vid2vga-capture PRIVATE setupapi)
endif()

set_target_properties(p2000t-vid2vga-capture PROPERTIES
    WIN32_EXECUTABLE TRUE
)

install(TARGETS p2000t-vid2vga-capture RUNTIME DESTINATION .)

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
