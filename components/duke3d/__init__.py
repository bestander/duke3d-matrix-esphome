import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

duke3d_ns = cg.esphome_ns.namespace("duke3d")
Duke3DClass = duke3d_ns.class_("Duke3DComponent", cg.Component)

CONF_SMOKE_TEST      = "smoke_test"
CONF_USB_GAMEPAD     = "usb_gamepad"
CONF_TILE_CACHE      = "tile_cache"
CONF_PAUSE_WIFI      = "pause_wifi"
CONF_WIFI_INTERVAL_S = "wifi_interval_s"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Duke3DClass),
    cv.Optional(CONF_SMOKE_TEST,       default=False): cv.boolean,
    cv.Optional(CONF_USB_GAMEPAD,      default=False): cv.boolean,
    cv.Optional(CONF_TILE_CACHE,       default=True):  cv.boolean,
    cv.Optional(CONF_PAUSE_WIFI,       default=False): cv.boolean,
    cv.Optional(CONF_WIFI_INTERVAL_S,  default=3600):  cv.positive_int,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_smoke_test(config[CONF_SMOKE_TEST]))
    cg.add(var.set_usb_gamepad(config[CONF_USB_GAMEPAD]))
    cg.add(var.set_tile_cache(config[CONF_TILE_CACHE]))
    cg.add(var.set_pause_wifi(config[CONF_PAUSE_WIFI]))
    cg.add(var.set_wifi_interval_s(config[CONF_WIFI_INTERVAL_S]))
    await cg.register_component(var, config)
