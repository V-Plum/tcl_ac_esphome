import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart
from esphome.const import CONF_ID, CONF_UART_ID

AUTO_LOAD = ["climate", "uart"]

tcl_ac_ns = cg.esphome_ns.namespace("tcl_ac")
TCLACClimate = tcl_ac_ns.class_(
    "TCLACClimate", climate.Climate, cg.Component, uart.UARTDevice
)

# climate_schema(class) is the modern ESPHome API (2024+).
# It auto-generates the entity ID for TCLACClimate, replacing the old getattr hack.
CONFIG_SCHEMA = (
    climate.climate_schema(TCLACClimate)
    .extend({
        cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Optional("object_id"): cv.string,
    })
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    await uart.register_uart_device(var, config)
