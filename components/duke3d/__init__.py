import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import time as time_comp
from esphome.const import CONF_ID

duke3d_ns = cg.esphome_ns.namespace("duke3d")
Duke3DClass = duke3d_ns.class_("Duke3DComponent", cg.Component)

CONF_SMOKE_TEST              = "smoke_test"
CONF_USB_GAMEPAD             = "usb_gamepad"
CONF_TILE_CACHE              = "tile_cache"
CONF_PAUSE_WIFI              = "pause_wifi"
CONF_WIFI_BOOTSTRAP_GRACE_S  = "wifi_bootstrap_grace_s"
CONF_WIFI_SYNC_MIN_INTERVAL_S = "wifi_sync_min_interval_s"
CONF_WIFI_HA_SYNC              = "wifi_ha_sync"
CONF_TIME_ID                 = "time_id"
CONF_AUDIO_OUTPUT_PERCENT    = "audio_output_percent"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Duke3DClass),
    cv.Optional(CONF_SMOKE_TEST,               default=False): cv.boolean,
    cv.Optional(CONF_USB_GAMEPAD,              default=False): cv.boolean,
    cv.Optional(CONF_TILE_CACHE,               default=True):  cv.boolean,
    cv.Optional(CONF_PAUSE_WIFI,               default=False): cv.boolean,
    cv.Optional(CONF_WIFI_BOOTSTRAP_GRACE_S,   default=12):   cv.positive_int,
    cv.Optional(CONF_WIFI_SYNC_MIN_INTERVAL_S, default=90):   cv.positive_int,
    cv.Optional(CONF_WIFI_HA_SYNC, default=True): cv.boolean,
    cv.Optional(CONF_TIME_ID): cv.use_id(time_comp.RealTimeClock),
    cv.Optional(CONF_AUDIO_OUTPUT_PERCENT, default=50): cv.All(
        cv.int_, cv.Range(min=0, max=100)
    ),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_smoke_test(config[CONF_SMOKE_TEST]))
    cg.add(var.set_usb_gamepad(config[CONF_USB_GAMEPAD]))
    cg.add(var.set_tile_cache(config[CONF_TILE_CACHE]))
    cg.add(var.set_pause_wifi(config[CONF_PAUSE_WIFI]))
    cg.add(var.set_wifi_bootstrap_grace_s(config[CONF_WIFI_BOOTSTRAP_GRACE_S]))
    cg.add(var.set_wifi_sync_min_interval_s(config[CONF_WIFI_SYNC_MIN_INTERVAL_S]))
    cg.add(var.set_wifi_ha_sync(config[CONF_WIFI_HA_SYNC]))
    cg.add(var.set_audio_output_percent(config[CONF_AUDIO_OUTPUT_PERCENT]))
    if CONF_TIME_ID in config:
        t = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(t))
    await cg.register_component(var, config)
