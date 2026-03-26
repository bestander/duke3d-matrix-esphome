import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

duke3d_ns = cg.esphome_ns.namespace("duke3d")
Duke3DClass = duke3d_ns.class_("Duke3DComponent", cg.Component)

CONF_SMOKE_TEST = "smoke_test"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Duke3DClass),
    cv.Optional(CONF_SMOKE_TEST, default=False): cv.boolean,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_smoke_test(config[CONF_SMOKE_TEST]))
    await cg.register_component(var, config)
