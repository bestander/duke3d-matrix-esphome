set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Engine")
set(GAME_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Game")
set(SDL_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/SDL")

# Engine core: all .c files.
# Exclude dummy_multi.c — mmulti.c provides both the variable definitions AND
# STUB_NETWORKING stubs, so including both would cause duplicate symbols.
file(GLOB ENGINE_SRCS "${ENGINE_DIR}/*.c")
list(FILTER ENGINE_SRCS EXCLUDE REGEX ".*/dummy_multi\\.c$")

# Game layer: all .c files (includes dummy_audiolib.c which stubs sound).
file(GLOB GAME_SRCS "${GAME_DIR}/*.c")

# SDL shim: all .c files except spi_lcd.c — we provide our own implementation
# in esp32_hal.cpp that renders to HUB75 instead of SPI LCD.
file(GLOB SDL_SRCS "${SDL_DIR}/*.c")
list(FILTER SDL_SRCS EXCLUDE REGEX ".*/spi_lcd\\.c$")

idf_component_register(
    SRCS
        "duke3d_component.cpp"
        "renderer.cpp"
        "esp32_hal.cpp"
        "tilecache.cpp"
        "input.cpp"
        "usb_gamepad.cpp"
        "engine_main_shim.c"
        ${ENGINE_SRCS}
        ${GAME_SRCS}
        ${SDL_SRCS}
    INCLUDE_DIRS
        "."
        "${ENGINE_DIR}"
        "${SDL_DIR}"
        "${GAME_DIR}"
        "${ENGINE_DIR}/enet/include"
    COMPILE_DEFINITIONS
        DUKE3D_ENGINE_PRESENT
    REQUIRES hub75_matrix hud sd_card i2s_audio freertos esp_task_wdt esp_wifi usb driver
)

# mmulti.c includes <enet/enet.h> unconditionally via HAVE_ENET_ENET_H.
# We use STUB_NETWORKING, so enet is never called — suppress the include.
set_source_files_properties(
    "${ENGINE_DIR}/mmulti.c"
    PROPERTIES COMPILE_FLAGS "-UHAVE_ENET_ENET_H -DHAVE_ENET_ENET_H=0"
)

# network.c provides sendpacket/flushpackets/sendlogoff (routing to mmulti.c stubs).
# It also defines callcommit, which conflicts with mmulti.c's STUB_NETWORKING empty stub.
# Rename it — mmulti.c's no-op is correct for single-player/demo mode.
set_source_files_properties(
    "${ENGINE_DIR}/network.c"
    PROPERTIES COMPILE_FLAGS "-Dcallcommit=_network_callcommit_unused"
)
