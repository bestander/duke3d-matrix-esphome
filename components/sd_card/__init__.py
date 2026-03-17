import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MOSI_PIN, CONF_MISO_PIN, CONF_CLK_PIN, CONF_CS_PIN

sd_ns = cg.esphome_ns.namespace("sd_card")
SdCardClass = sd_ns.class_("SdCard", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SdCardClass),
    cv.Required(CONF_MOSI_PIN): cv.positive_int,
    cv.Required(CONF_MISO_PIN): cv.positive_int,
    cv.Required(CONF_CLK_PIN):  cv.positive_int,
    cv.Required(CONF_CS_PIN):   cv.positive_int,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_mosi(config[CONF_MOSI_PIN]))
    cg.add(var.set_miso(config[CONF_MISO_PIN]))
    cg.add(var.set_sck(config[CONF_CLK_PIN]))
    cg.add(var.set_cs(config[CONF_CS_PIN]))
    await cg.register_component(var, config)
