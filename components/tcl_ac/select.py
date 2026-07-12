import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID, CONF_TYPE

from .climate import TCLACClimate, tcl_ac_ns

AUTO_LOAD = ["select"]

CONF_TCL_AC_ID = "tcl_ac_id"

TCLACSelect = tcl_ac_ns.class_("TCLACSelect", select.Select)
TCLACSelectType = tcl_ac_ns.enum("TCLACSelectType")

SELECT_TYPES = {
    "vertical_louver":   TCLACSelectType.LOUVER_V,
    "horizontal_louver": TCLACSelectType.LOUVER_H,
}

# Options must match the index encoding in tcl_ac.h (TCLACSelect comment)
V_OPTIONS = [
    "Вимкнено",
    "Гойдання: повне",
    "Гойдання: верхнє",
    "Гойдання: нижнє",
    "Фіксація: верх",
    "Фіксація: верхня",
    "Фіксація: центр",
    "Фіксація: нижня",
    "Фіксація: низ",
]

H_OPTIONS = [
    "Вимкнено",
    "Гойдання: повне",
    "Гойдання: ліво",
    "Гойдання: центр",
    "Гойдання: право",
    "Фіксація: крайній ліворуч",
    "Фіксація: ліворуч",
    "Фіксація: центр",
    "Фіксація: праворуч",
    "Фіксація: крайній праворуч",
]

CONFIG_SCHEMA = select.select_schema(TCLACSelect).extend(
    {
        cv.GenerateID(CONF_TCL_AC_ID): cv.use_id(TCLACClimate),
        cv.Required(CONF_TYPE): cv.one_of(*SELECT_TYPES.keys(), lower=True),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TCL_AC_ID])
    var = cg.new_Pvariable(config[CONF_ID])

    sw_type = config[CONF_TYPE]
    options = V_OPTIONS if sw_type == "vertical_louver" else H_OPTIONS

    await select.register_select(var, config, options=options)

    cg.add(var.set_parent(parent))
    cg.add(var.set_type(SELECT_TYPES[sw_type]))

    if sw_type == "vertical_louver":
        cg.add(parent.set_vertical_louver_select(var))
    else:
        cg.add(parent.set_horizontal_louver_select(var))
