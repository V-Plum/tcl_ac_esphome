import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_TYPE

from .climate import TCLACClimate, tcl_ac_ns

CONF_TCL_AC_ID = "tcl_ac_id"

TCLACSwitch = tcl_ac_ns.class_("TCLACSwitch", switch.Switch)
TCLACSwitchType = tcl_ac_ns.enum("TCLACSwitchType")

SWITCH_TYPES = {
    "display":      TCLACSwitchType.DISPLAY_ON,
    "beeper":       TCLACSwitchType.BEEPER_ON,
    "gentle_wind":  TCLACSwitchType.GENTLE_WIND,
}

CONFIG_SCHEMA = switch.switch_schema(TCLACSwitch).extend(
    {
        cv.GenerateID(CONF_TCL_AC_ID): cv.use_id(TCLACClimate),
        cv.Required(CONF_TYPE): cv.one_of(*SWITCH_TYPES.keys(), lower=True),
        cv.Optional("object_id"): cv.string,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TCL_AC_ID])
    var = cg.new_Pvariable(config[CONF_ID])

    await switch.register_switch(var, config)

    cg.add(var.set_parent(parent))
    cg.add(var.set_type(SWITCH_TYPES[config[CONF_TYPE]]))

    if config[CONF_TYPE] == "display":
        cg.add(parent.set_display_switch(var))
    elif config[CONF_TYPE] == "beeper":
        cg.add(parent.set_beeper_switch(var))
    else:
        cg.add(parent.set_gentle_wind_switch(var))
