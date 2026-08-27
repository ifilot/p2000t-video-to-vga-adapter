# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

set(FIRMWARE_TARGET p2000t_vid2vga_firmware)

set(NO_CONNECTION_IMAGE_ASSEMBLIES)
function(embed_no_connection_image symbol relative_source)
    set(NO_CONNECTION_IMAGE_SYMBOL "${symbol}")
    set(NO_CONNECTION_IMAGE_SOURCE "${CMAKE_SOURCE_DIR}/${relative_source}")
    set(NO_CONNECTION_IMAGE_ASSEMBLY
        "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.S")
    if(NOT EXISTS "${NO_CONNECTION_IMAGE_SOURCE}")
        message(FATAL_ERROR
            "No-connection artwork not found: ${NO_CONNECTION_IMAGE_SOURCE}")
    endif()
    configure_file(no_connection_image.S.in
        "${NO_CONNECTION_IMAGE_ASSEMBLY}" @ONLY)
    set_source_files_properties("${NO_CONNECTION_IMAGE_ASSEMBLY}" PROPERTIES
        OBJECT_DEPENDS "${NO_CONNECTION_IMAGE_SOURCE}"
    )
    list(APPEND NO_CONNECTION_IMAGE_ASSEMBLIES
        "${NO_CONNECTION_IMAGE_ASSEMBLY}")
    set(NO_CONNECTION_IMAGE_ASSEMBLIES
        "${NO_CONNECTION_IMAGE_ASSEMBLIES}" PARENT_SCOPE)
endfunction()

embed_no_connection_image(no_connection_image_green_phosphor
    "assets/no_connection/green_phosphor.idx4")
embed_no_connection_image(no_connection_image_synthwave
    "assets/no_connection/synthwave.idx4")
embed_no_connection_image(no_connection_image_amber_circuit
    "assets/no_connection/amber_circuit.idx4")

add_executable(${FIRMWARE_TARGET}
    firmware.c
    ${NO_CONNECTION_IMAGE_ASSEMBLIES}
    p2000t_capture.c
    p2000t_video_renderer.c
)

target_include_directories(${FIRMWARE_TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

set_target_properties(${FIRMWARE_TARGET} PROPERTIES
    OUTPUT_NAME "p2000t-vid2vga-firmware"
)

pico_generate_pio_header(${FIRMWARE_TARGET}
    ${CMAKE_CURRENT_LIST_DIR}/p2000t_capture.pio
)

target_link_libraries(${FIRMWARE_TARGET} PRIVATE
    pico_stdlib
    pico_stdio_usb
    pico_multicore
    pico_scanvideo_dpi
    hardware_clocks
    hardware_dma
    hardware_pio
    hardware_sync
    hardware_vreg
)

target_compile_definitions(${FIRMWARE_TARGET} PRIVATE
    P2000T_VID2VGA_VERSION="${PROJECT_VERSION}"

    PICO_SCANVIDEO_COLOR_PIN_BASE=0
    PICO_SCANVIDEO_COLOR_PIN_COUNT=12
    PICO_SCANVIDEO_SYNC_PIN_BASE=12

    PICO_SCANVIDEO_DPI_PIXEL_RSHIFT=0
    PICO_SCANVIDEO_DPI_PIXEL_GSHIFT=4
    PICO_SCANVIDEO_DPI_PIXEL_BSHIFT=8
    PICO_SCANVIDEO_DPI_PIXEL_RCOUNT=4
    PICO_SCANVIDEO_DPI_PIXEL_GCOUNT=4
    PICO_SCANVIDEO_DPI_PIXEL_BCOUNT=4

    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT=8
)

if(P2000T_PROTOTYPE_V1_MIRRORED_DIN)
    target_compile_definitions(${FIRMWARE_TARGET} PRIVATE
        P2000T_PROTOTYPE_V1_MIRRORED_DIN=1)
else()
    target_compile_definitions(${FIRMWARE_TARGET} PRIVATE
        P2000T_PROTOTYPE_V1_MIRRORED_DIN=0)
endif()

set_source_files_properties(
    firmware.c
    p2000t_capture.c
    p2000t_video_renderer.c
    PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic"
)

pico_enable_stdio_uart(${FIRMWARE_TARGET} 0)
pico_enable_stdio_usb(${FIRMWARE_TARGET} 1)
pico_add_extra_outputs(${FIRMWARE_TARGET})
