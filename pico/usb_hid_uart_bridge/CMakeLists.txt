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
  ${CMAKE_CURRENT_LIST_DIR}/../../components/duke3d
)

target_link_libraries(usb_hid_uart_bridge
  pico_stdlib
  tinyusb_host
  tinyusb_board
  hardware_uart
)

if (PICO_CYW43_SUPPORTED)
  target_link_libraries(usb_hid_uart_bridge
    pico_cyw43_arch_none
  )
endif()

# USB device (CDC) stdio clashes with TinyUSB USB host on RP2040 — keep stdio off UART/USB.
# Debug HID dumps use uart_write_blocking with "[hid]" / "[bridge]" prefixes; ESP ignores those lines.
pico_enable_stdio_usb(usb_hid_uart_bridge 0)
pico_enable_stdio_uart(usb_hid_uart_bridge 0)
if(NOT DEFINED PICO_BRIDGE_DEBUG)
  set(PICO_BRIDGE_DEBUG 1)
endif()
target_compile_definitions(usb_hid_uart_bridge PRIVATE
  PICO_BRIDGE_DEBUG=${PICO_BRIDGE_DEBUG}
  CFG_TUSB_DEBUG=0
)

pico_add_extra_outputs(usb_hid_uart_bridge)
