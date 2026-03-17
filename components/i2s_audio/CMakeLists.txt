idf_component_register(
    SRCS "i2s_audio.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2s freertos
    # esp_driver_i2s: explicit dependency on new channel API (driver/i2s_std.h).
    # The "driver" component re-exports it but listing it explicitly avoids
    # ambiguity with ESPHome's own i2s_audio component which uses the legacy API.
    # Do NOT add ESPHome's built-in i2s_audio platform to esphome.yaml alongside this.
)
