import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

i2s_ns = cg.esphome_ns.namespace("i2s_audio")
I2SAudioClass = i2s_ns.class_("I2SAudio", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(I2SAudioClass),
    cv.Required("bclk_pin"):  cv.positive_int,
    cv.Required("lrclk_pin"): cv.positive_int,
    cv.Required("din_pin"):   cv.positive_int,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_bclk(config["bclk_pin"]))
    cg.add(var.set_lrclk(config["lrclk_pin"]))
    cg.add(var.set_din(config["din_pin"]))
    await cg.register_component(var, config)
