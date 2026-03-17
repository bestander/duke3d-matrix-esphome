# Add Duke3D engine source files.
# After adding jkirsons/Duke3D submodule in Task 5.1, update these globs
# to match the engine's component directory layout.
file(GLOB_RECURSE ENGINE_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Duke3D/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Duke3D/*.cpp"
)

idf_component_register(
    SRCS
        "duke3d_component.cpp"
        "renderer/renderer.cpp"
        "platform/esp32_hal.cpp"
        "platform/input.cpp"
        ${ENGINE_SRCS}
    INCLUDE_DIRS
        "."
        "renderer"
        "platform"
        "engine/components/Duke3D"
        "engine/components/Duke3D/Engine"
    REQUIRES hub75_matrix hud sd_card i2s_audio freertos esp_task_wdt
)
