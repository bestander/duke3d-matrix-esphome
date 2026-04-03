set(ENGINE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Engine")
set(GAME_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Game")
set(SDL_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/SDL")
set(AUDIOLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/audiolib")

# Engine core: all .c files.
# Exclude dummy_multi.c — mmulti.c provides both the variable definitions AND
# STUB_NETWORKING stubs, so including both would cause duplicate symbols.
file(GLOB ENGINE_SRCS "${ENGINE_DIR}/*.c")
list(FILTER ENGINE_SRCS EXCLUDE REGEX ".*/dummy_multi\\.c$")

# Game layer: all .c files.
# dummy_audiolib.c is entirely commented out — harmless but kept for completeness.
file(GLOB GAME_SRCS "${GAME_DIR}/*.c")

# SDL shim: all .c files except spi_lcd.c (we use HUB75) and SDL_audio.c
# (audio is driven by our own pump task in dsl.c, not by SDL_OpenAudio).
file(GLOB SDL_SRCS "${SDL_DIR}/*.c")
list(FILTER SDL_SRCS EXCLUDE REGEX ".*/spi_lcd\\.c$")
list(FILTER SDL_SRCS EXCLUDE REGEX ".*/SDL_audio\\.c$")

# Apogee Multivoc audiolib: real FX mixing.
# Exclude DOS ISA hardware drivers, music/OPL2, and the no-sound stubs
# (nomusic.c includes music.h -> duke3d.h which re-defines large globals).
file(GLOB AUDIOLIB_SRCS "${AUDIOLIB_DIR}/*.c")
foreach(_excl
    adlibfx al_midi awe32 blaster debugio dma dpmi
    gus gusmidi guswave irq leetimbr midi mpu401 music
    nomusic pas16 sndscape sndsrc task_man gmtimbre)
    list(FILTER AUDIOLIB_SRCS EXCLUDE REGEX ".*/${_excl}\\.c$")
endforeach()

idf_component_register(
    SRCS
        "duke3d_component.cpp"
        "renderer.cpp"
        "esp32_hal.cpp"
        "tilecache.cpp"
        "input.cpp"
        "usb_gamepad.cpp"
        "engine_main_shim.c"
        "audiolib_stubs.c"
        ${ENGINE_SRCS}
        ${GAME_SRCS}
        ${SDL_SRCS}
        ${AUDIOLIB_SRCS}
    INCLUDE_DIRS
        "."
        "${ENGINE_DIR}"
        "${SDL_DIR}"
        "${GAME_DIR}"
        "${AUDIOLIB_DIR}"
        "${ENGINE_DIR}/enet/include"
    COMPILE_DEFINITIONS
        DUKE3D_ENGINE_PRESENT
        DUKE3D_TILE_ASYNC_SPIKE
    REQUIRES hub75_matrix hud sd_card i2s_audio freertos esp_task_wdt esp_wifi wifi usb driver
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
