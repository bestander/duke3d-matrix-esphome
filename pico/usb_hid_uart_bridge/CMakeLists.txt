cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

# Default to Pico W; override with -DPICO_BOARD=... if needed.
if (NOT PICO_BOARD)
  set(PICO_BOARD pico_w)
endif()

project(usb_hid_uart_bridge C CXX ASM)

pico_sdk_init()

add_executable(usb_hid_uart_bridge
  main.c
)

target_include_directories(usb_hid_uart_bridge PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usb_hid_uart_bridge
  pico_stdlib
  tinyusb_host
  tinyusb_board
  hardware_uart
)

target_compile_definitions(usb_hid_uart_bridge PRIVATE
  PICO_STDIO_USB=1
  PICO_STDIO_UART=0
)

pico_add_extra_outputs(usb_hid_uart_bridge)
