import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = []
CODEOWNERS = []

stdout_fix_ns = cg.esphome_ns.namespace("stdout_fix")
StdoutFix = stdout_fix_ns.class_("StdoutFix", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(StdoutFix),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
