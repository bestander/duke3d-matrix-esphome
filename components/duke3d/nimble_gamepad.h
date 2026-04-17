#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Set target BLE gamepad MAC address (AA:BB:CC:DD:EE:FF). Optional but
// recommended; when unset, NimBLE init logs an error and does not scan.
void nimble_gamepad_set_target_mac(const char *mac);

// Call once from Duke3DComponent::setup(), after WiFi bootstrap config and
// before the game task starts.
void nimble_gamepad_init(void);

#ifdef __cplusplus
}
#endif
