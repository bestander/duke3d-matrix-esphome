import pathlib

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import time as time_comp
from esphome.const import CONF_ID

duke3d_ns = cg.esphome_ns.namespace("duke3d")
Duke3DClass = duke3d_ns.class_("Duke3DComponent", cg.Component)

CONF_SMOKE_TEST              = "smoke_test"
CONF_TILE_CACHE              = "tile_cache"
CONF_FLASH_TILES             = "flash_tiles"
CONF_PAUSE_WIFI              = "pause_wifi"
CONF_WIFI_BOOTSTRAP_GRACE_S  = "wifi_bootstrap_grace_s"
CONF_WIFI_SYNC_MIN_INTERVAL_S = "wifi_sync_min_interval_s"
CONF_WIFI_HA_SYNC              = "wifi_ha_sync"
CONF_TIME_ID                 = "time_id"
CONF_AUDIO_OUTPUT_PERCENT    = "audio_output_percent"
CONF_PICO_UART_INPUT         = "pico_uart_input"
CONF_PICO_UART_NUM           = "pico_uart_num"
CONF_PICO_UART_TX_PIN        = "pico_uart_tx_pin"
CONF_PICO_UART_RX_PIN        = "pico_uart_rx_pin"
CONF_PICO_UART_BAUD_RATE     = "pico_uart_baud_rate"
CONF_PICO_GAMEPAD_MAP        = "pico_gamepad_map"
CONF_REPORT                  = "report"
CONF_ACTION                  = "action"

# Logical gamepad controls (matches Pico decode bits in main.c).
PICO_GAMEPAD_MAP_KEYS = (
    "cross_up",
    "cross_down",
    "cross_left",
    "cross_right",
    "a",
    "b",
    "c",
    "x",
    "y",
    "z",
    "start",
    "bumper_l",
    "bumper_r",
    "star",
    "dash",
    "heart",
)

# Duke scancodes (keyboard.h) for each YAML `action:` token.
DUKE_SCAN_BY_ACTION = {
    "none": 0x00,
    "arrow_up": 0x5A,
    "arrow_down": 0x6A,
    "arrow_left": 0x6B,
    "arrow_right": 0x6C,
    "strafe_mod": 0x38,   # Left Alt (Duke Strafe default)
    "open": 0x39,         # Space
    "jump": 0x1E,         # A (Duke Jump default)
    "crouch": 0x2C,       # Z (Duke Crouch default)
    "shoot": 0x1D,        # Left Ctrl
    "next_weapon": 0x28,  # '
    "inventory": 0x1C,    # Enter
    "inventory_next": 0x1B,  # ]
    "escape": 0x01,
    # Start decoded from GR on ESP — `game_task` runs live E1L1 warp; no Duke KB bytes from Pico for Start.
    "play_live": None,
    # Deprecated synonym for play_live on pico_gamepad_map.start only (YAML compatibility).
    "record_session": None,
}

# Enables DROP_HID_ESC + Start latch polling (pads often emit Start as HID Escape).
PICO_UART_START_SPECIAL_ACTIONS = frozenset({"play_live", "record_session"})

DUKE_GAMEPAD_ACTIONS_LIST = tuple(DUKE_SCAN_BY_ACTION.keys())

# Default gameplay mapping (used when an entry omits `action:`).
DEFAULT_GAMEPAD_ACTIONS = {
    "cross_up": "arrow_up",
    "cross_down": "arrow_down",
    "cross_left": "arrow_left",
    "cross_right": "arrow_right",
    "a": "shoot",
    "b": "next_weapon",
    "c": "crouch",
    "x": "inventory",
    "y": "inventory_next",
    "z": "jump",
    "start": "play_live",
    "bumper_l": "strafe_mod",
    "bumper_r": "open",
    "star": "escape",
    "dash": "none",
    "heart": "none",
}

GAMEPAD_BTN_ENTRY_SCHEMA = cv.Any(
    cv.Schema(
        {
            cv.Optional(CONF_REPORT, default=""): cv.string,
            cv.Optional(CONF_ACTION): cv.one_of(*DUKE_GAMEPAD_ACTIONS_LIST),
        }
    ),
    cv.string,
)


def _normalize_gamepad_map(value):
    """Ensure every logical button exists with report + resolved action."""
    value = value or {}
    out = {}
    for key in PICO_GAMEPAD_MAP_KEYS:
        entry = value.get(key)
        if entry is None:
            report = ""
            action = DEFAULT_GAMEPAD_ACTIONS[key]
        elif isinstance(entry, str):
            report = entry.strip()
            action = DEFAULT_GAMEPAD_ACTIONS[key]
        else:
            report = (entry.get(CONF_REPORT) or "").strip()
            action = entry.get(CONF_ACTION)
            if action is None:
                action = DEFAULT_GAMEPAD_ACTIONS[key]
        if action not in DUKE_SCAN_BY_ACTION:
            raise cv.Invalid(f"pico_gamepad_map.{key}: invalid action '{action}'")
        out[key] = {CONF_REPORT: report, CONF_ACTION: action}
    for key in PICO_GAMEPAD_MAP_KEYS:
        if out[key][CONF_ACTION] in PICO_UART_START_SPECIAL_ACTIONS and key != "start":
            raise cv.Invalid("actions 'play_live' and 'record_session' are only valid for pico_gamepad_map.start")
    return out


PICO_GAMEPAD_MAP_SCHEMA = cv.All(
    cv.Schema({cv.Optional(key): GAMEPAD_BTN_ENTRY_SCHEMA for key in PICO_GAMEPAD_MAP_KEYS}),
    _normalize_gamepad_map,
)

PICO_UART_BRIDGE_MAPS_H = pathlib.Path(__file__).resolve().parent / "pico_uart_bridge_maps.h"


# TinyUSB/USB HID keyboard usage IDs for keys we forward as KB,... from the Pico bridge.
_HID_DUKE = (
    # (hid_usage_hex, duke_keyboard_scancode)
    (0x1A, 0x11),  # W
    (0x04, 0x1E),  # A
    (0x16, 0x1F),  # S
    (0x07, 0x20),  # D
    (0x1D, 0x2C),  # Z
    (0x52, 0x5A),  # Up arrow
    (0x51, 0x6A),  # Down
    (0x50, 0x6B),  # Left
    (0x4F, 0x6C),  # Right
    (0x2C, 0x39),  # Space
    (0x2B, 0x0F),  # Tab
    (0x28, 0x1C),  # Enter
    (0x29, 0x01),  # Escape → Duke ESC (unless DROP_HID_KEYBOARD_ESCAPE)
    (0xE0, 0x1D),  # Left Control
)


def _write_pico_uart_bridge_maps_header(normalized_map):
    """ESP-only: Pico sends GR,... (raw HID) and KB,<hid>,<0|1>; this maps logical/HID → Duke scans."""
    lines = [
        "/* Auto-generated by ESPHome components/duke3d — do not edit by hand.",
        " * Source: esphome.yaml → duke3d.pico_gamepad_map + fixed HID keyboard subset.",
        " * Pico firmware has no Duke knowledge; regenerate after `esphome compile`.",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "#include <string.h>",
        "",
    ]
    drop_esc = normalized_map["start"][CONF_ACTION] in PICO_UART_START_SPECIAL_ACTIONS
    lines.append(
        "/* When start.action is play_live (or legacy record_session): drop HID ESC (pads leak Start as ESC). */"
    )
    lines.append(f"#define PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE ({1 if drop_esc else 0}u)")
    lines.append("")
    lines.append('#define PICO_UART_BRIDGE_HID_KEYBOARD_ESCAPE (0x29u)')
    lines.append("")
    lines.append(
        '/* Returned scancode [0..] or negative: -1 → no Duke mapping (ignore).\n'
        ' * Logical names match vendor decode bitmask → string (YAML pico_gamepad_map keys).\n */\n'
        "static inline int32_t pico_uart_gp_logical_to_duke_scancode(const char *n) {"
    )
    lines.append("    if (!n)")
    lines.append("        return -1;")
    for key in PICO_GAMEPAD_MAP_KEYS:
        action = normalized_map[key][CONF_ACTION]
        if action in ("record_session", "play_live", "none") or DUKE_SCAN_BY_ACTION[action] in (None, 0):
            lines.append(f'    if (!strcmp(n, "{key}"))')
            lines.append("        return -1;")
            continue
        sc = DUKE_SCAN_BY_ACTION[action]
        lines.append(f'    if (!strcmp(n, "{key}"))')
        lines.append(f"        return {int(sc)};")
    lines.extend(["    return -1;", "}"])
    lines.append("")
    lines.append(
        "/* USB HID boot keyboard usage in KB,... second field → Duke scancode or -1 if unmapped */\n"
        "static inline int32_t pico_uart_hid_keyboard_to_duke_scancode(unsigned hid) {"
    )
    lines.append("    switch (hid) {")
    for hid, sc in _HID_DUKE:
        lines.append(f"    case 0x{hid:02X}u:")
        lines.append(f"        return {int(sc)};")
    lines.extend(
        ["    default:", "        return -1;", "    }", "}"],
    )
    lines.append("")
    PICO_UART_BRIDGE_MAPS_H.write_text("\n".join(lines), encoding="utf-8")


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Duke3DClass),
        cv.Optional(CONF_SMOKE_TEST, default=False): cv.boolean,
        cv.Optional(CONF_TILE_CACHE, default=True): cv.boolean,
        cv.Optional(CONF_FLASH_TILES, default=False): cv.boolean,
        cv.Optional(CONF_PAUSE_WIFI, default=False): cv.boolean,
        cv.Optional(CONF_WIFI_BOOTSTRAP_GRACE_S, default=12): cv.positive_int,
        cv.Optional(CONF_WIFI_SYNC_MIN_INTERVAL_S, default=90): cv.positive_int,
        cv.Optional(CONF_WIFI_HA_SYNC, default=True): cv.boolean,
        cv.Optional(CONF_TIME_ID): cv.use_id(time_comp.RealTimeClock),
        cv.Optional(CONF_AUDIO_OUTPUT_PERCENT, default=50): cv.All(cv.int_, cv.Range(min=0, max=100)),
        cv.Optional(CONF_PICO_UART_INPUT, default=False): cv.boolean,
        cv.Optional(CONF_PICO_UART_NUM, default=1): cv.int_range(min=0, max=2),
        cv.Optional(CONF_PICO_UART_TX_PIN, default=17): cv.int_,
        cv.Optional(CONF_PICO_UART_RX_PIN, default=16): cv.int_,
        cv.Optional(CONF_PICO_UART_BAUD_RATE, default=115200): cv.positive_int,
        cv.Optional(CONF_PICO_GAMEPAD_MAP): PICO_GAMEPAD_MAP_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_smoke_test(config[CONF_SMOKE_TEST]))
    cg.add(var.set_tile_cache(config[CONF_TILE_CACHE]))
    if config[CONF_FLASH_TILES]:
        cg.add_define("DUKE3D_FLASH_TILES")
    cg.add(var.set_pause_wifi(config[CONF_PAUSE_WIFI]))
    cg.add(var.set_wifi_bootstrap_grace_s(config[CONF_WIFI_BOOTSTRAP_GRACE_S]))
    cg.add(var.set_wifi_sync_min_interval_s(config[CONF_WIFI_SYNC_MIN_INTERVAL_S]))
    cg.add(var.set_wifi_ha_sync(config[CONF_WIFI_HA_SYNC]))
    cg.add(var.set_audio_output_percent(config[CONF_AUDIO_OUTPUT_PERCENT]))
    cg.add(var.set_pico_uart_input(config[CONF_PICO_UART_INPUT]))
    cg.add(var.set_pico_uart_num(config[CONF_PICO_UART_NUM]))
    cg.add(var.set_pico_uart_tx_pin(config[CONF_PICO_UART_TX_PIN]))
    cg.add(var.set_pico_uart_rx_pin(config[CONF_PICO_UART_RX_PIN]))
    cg.add(var.set_pico_uart_baud_rate(config[CONF_PICO_UART_BAUD_RATE]))
    if CONF_TIME_ID in config:
        t = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(t))

    normalized = config.get(CONF_PICO_GAMEPAD_MAP)
    if normalized is None:
        normalized = _normalize_gamepad_map({})
    _write_pico_uart_bridge_maps_header(normalized)

    await cg.register_component(var, config)
