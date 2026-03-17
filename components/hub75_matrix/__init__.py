import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

hub75_ns = cg.esphome_ns.namespace("hub75_matrix")
Hub75MatrixClass = hub75_ns.class_("Hub75Matrix", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Hub75MatrixClass),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
