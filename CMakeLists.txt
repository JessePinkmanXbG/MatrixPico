# Matrix Pico - serial-tunable RGH 1.2 glitcher
cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(MatrixPico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Test-variant builds: MP_DEFS="MP_SYS_CLK_KHZ=200000;MP_RST_DRIVEN=1"
if(DEFINED ENV{MP_DEFS})
  add_compile_definitions($ENV{MP_DEFS})
endif()

add_executable(${PROJECT_NAME}
    main.c
)

pico_add_extra_outputs(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_LIST_DIR})

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/glitch.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/extclk.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    pico_multicore
    hardware_gpio
    hardware_pio
    hardware_flash
    hardware_sync
)

# Serial console lives on USB CDC; UART off.
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)
