file(GLOB_RECURSE HUB75_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ESP32-HUB75-MatrixPanel-I2S-DMA/src/*.cpp"
)

idf_component_register(
    SRCS "hub75_matrix.cpp" ${HUB75_SRCS}
    INCLUDE_DIRS
        "."
        "../../libs/ESP32-HUB75-MatrixPanel-I2S-DMA/src"
    REQUIRES esp_psram freertos driver
)

# ESPHome cg.add_define() applies to the main app, not this IDF component — hub75_matrix.cpp
# must see HUB75_HAS_SPLASH when hub75_splash.gen.h exists (written by __init__.py).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hub75_splash.gen.h")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HUB75_HAS_SPLASH=1)
endif()
