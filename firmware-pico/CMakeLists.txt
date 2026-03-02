cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(vga_project C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(vga_project
    main.c
    vga_rgb.pio
)

pico_generate_pio_header(vga_project ${CMAKE_CURRENT_LIST_DIR}/vga_rgb.pio)

target_include_directories(vga_project PRIVATE headers)

target_link_libraries(vga_project
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_uart
    hardware_clocks      
)

pico_enable_stdio_uart(vga_project 1)
pico_enable_stdio_usb(vga_project 0)

pico_add_extra_outputs(vga_project)