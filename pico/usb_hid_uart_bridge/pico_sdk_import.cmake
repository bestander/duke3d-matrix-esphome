# This import helper mirrors the common Pico SDK project pattern:
#   include(pico_sdk_import.cmake)
#
# It requires PICO_SDK_PATH to point to a local pico-sdk checkout.

if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
  set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
  message(STATUS "Using PICO_SDK_PATH from environment ('${PICO_SDK_PATH}')")
endif()

if (NOT PICO_SDK_PATH)
  message(FATAL_ERROR
    "PICO_SDK_PATH is not set.\n"
    "Set it to your local pico-sdk path, for example:\n"
    "  export PICO_SDK_PATH=$HOME/pico/pico-sdk")
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

