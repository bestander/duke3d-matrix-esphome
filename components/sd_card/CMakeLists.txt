idf_component_register(
    SRCS "sd_card.cpp" "sd_open_trace.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_vfs_fat esp_timer vfs
    # esp_vfs_fat provides esp_vfs_fat_sdspi_mount in ESP-IDF 5.x
)
# Note: final link must pass -Wl,--wrap=_open_r (see esphome.yaml build_flags) so libc
# _open_r calls hit __wrap__open_r in sd_open_trace.c; INTERFACE from this static lib
# does not always propagate through ESPHome’s link line.
