## @file
## @brief Schema and code generation of the hub's `atlantic_heat_pump:` block.
## @ingroup hioc_codegen
##
## Polls the heating and hot-water energy counters of an Atlantic heat pump (node type 22, subtype 1)
## into two kWh sensors (atlantic_heat_pump.h). Read-only; see docs/configuration/atlantic-heat-pump.md.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_ENERGY,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_KILOWATT_HOURS,
)

from .hub_names import home_io_control_ns
from .hub_validators import validate_device_id

CONF_ATLANTIC_HEAT_PUMP = "atlantic_heat_pump"
CONF_IO_DEVICE_ID = "io_device_id"
CONF_HEATING_ENERGY = "heating_energy"
CONF_HOT_WATER_ENERGY = "hot_water_energy"

AtlanticHeatPumpEnergy = home_io_control_ns.class_(
    "AtlanticHeatPumpEnergy", cg.PollingComponent
)

_ENERGY_SENSOR_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    icon="mdi:heat-pump",
)


def _validate_has_sensor(config):
    if CONF_HEATING_ENERGY not in config and CONF_HOT_WATER_ENERGY not in config:
        raise cv.Invalid(
            f"'{CONF_ATLANTIC_HEAT_PUMP}' needs '{CONF_HEATING_ENERGY}' and/or '{CONF_HOT_WATER_ENERGY}'"
        )
    return config


def _validate_update_interval(value):
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds < 5 * 60 * 1000:
        raise cv.Invalid(
            "update_interval must be at least 5min: each read is a radio exchange on a duty-cycled band"
        )
    return value


ATLANTIC_HEAT_PUMP_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AtlanticHeatPumpEnergy),
            cv.Required(CONF_IO_DEVICE_ID): validate_device_id,
            cv.Optional("update_interval", default="1h"): _validate_update_interval,
            cv.Optional(CONF_HEATING_ENERGY): _ENERGY_SENSOR_SCHEMA,
            cv.Optional(CONF_HOT_WATER_ENERGY): _ENERGY_SENSOR_SCHEMA,
        }
    ),
    _validate_has_sensor,
)


async def to_code(config, hub):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_update_interval(config["update_interval"]))
    cg.add(var.set_parent(hub))
    cg.add(var.set_device_id(config[CONF_IO_DEVICE_ID]))
    if CONF_HEATING_ENERGY in config:
        sens = await sensor.new_sensor(config[CONF_HEATING_ENERGY])
        cg.add(var.set_heating_energy_sensor(sens))
    if CONF_HOT_WATER_ENERGY in config:
        sens = await sensor.new_sensor(config[CONF_HOT_WATER_ENERGY])
        cg.add(var.set_hot_water_energy_sensor(sens))
