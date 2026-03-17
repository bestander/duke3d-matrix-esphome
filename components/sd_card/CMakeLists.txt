idf_component_register(
    SRCS "sd_card.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_vfs_fat
    # esp_vfs_fat provides esp_vfs_fat_sdspi_mount in ESP-IDF 5.x
)
