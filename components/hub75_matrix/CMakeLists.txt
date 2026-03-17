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
