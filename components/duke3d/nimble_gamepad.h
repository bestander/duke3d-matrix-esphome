#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Set target BLE gamepad by advertised 128-bit service UUID
// (format: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX).
void nimble_gamepad_set_target_uuid(const char *uuid_str);

// Set target BLE gamepad by advertised device name (e.g. "ShanWan Q36").
void nimble_gamepad_set_target_name(const char *name);

// Call once from Duke3DComponent::setup(), after WiFi bootstrap config and
// before the game task starts.
void nimble_gamepad_init(void);

#ifdef __cplusplus
}
#endif
