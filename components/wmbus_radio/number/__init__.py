# SPDX-License-Identifier: GPL-3.0-or-later
#
# Optional runtime control of the RAM MQTT outbox capacity (see
# ../mqtt_outbox.cpp). mqtt_buffer_size in the main wmbus_radio: block sets
# the compiled ceiling; this entity lets that value be lowered (never raised
# past the ceiling) live, e.g. from ESPHome's own web_server: + auth:, which
# is the "lightweight authenticated portal" this project relies on instead of
# rolling its own HTTP server / auth handling.
#
# The C++ class (WMBusBufferCapacityNumber) lives in
# wmbus_buffer_capacity_number.h next to this file. It needs no explicit
# #include here: ESPHome auto-includes every header belonging to a loaded
# component/platform into the generated esphome.h, the same way component.h
# and packet.h reach the main wmbus_radio: platform without an `includes =`
# declaration.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import RadioComponent, radio_ns

DEPENDENCIES = ["wmbus_radio"]

CONF_WMBUS_RADIO_ID = "wmbus_radio_id"
CONF_BUFFER_CAPACITY = "buffer_capacity"

WMBusBufferCapacityNumber = radio_ns.class_(
    "WMBusBufferCapacityNumber", number.Number, cg.Component
)

# Ceiling for the number entity's slider/input. The *effective* ceiling is
# whatever mqtt_buffer_size compiled to on the wmbus_radio: block - the C++
# side (Radio::set_mqtt_outbox_capacity) clamps to that and republishes the
# clamped value, so setting this higher than mqtt_buffer_size is harmless,
# just a slider that can be dragged past the point where it stops taking
# effect. Matches the highest value mqtt_buffer_size/auto can ever compile
# to (_validate_mqtt_buffer_size's cv.int_range max, and
# MAX_SUGGESTED_CAPACITY in mqtt_outbox.cpp) so the slider can always reach
# the real ceiling instead of silently capping below it.
_MAX_BUFFER_CAPACITY = 8192  # PSRAM boards can go this high; without PSRAM the runtime reserve pins it far lower

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_WMBUS_RADIO_ID): cv.use_id(RadioComponent),
        cv.Optional(CONF_BUFFER_CAPACITY): number.number_schema(
            WMBusBufferCapacityNumber,
            icon="mdi:tray-full",
            entity_category=ENTITY_CATEGORY_CONFIG,
            unit_of_measurement="messages",
        ),
    }
)


async def to_code(config):
    radio = await cg.get_variable(config[CONF_WMBUS_RADIO_ID])

    if CONF_BUFFER_CAPACITY in config:
        conf = config[CONF_BUFFER_CAPACITY]
        var = await number.new_number(
            conf, min_value=0, max_value=_MAX_BUFFER_CAPACITY, step=1
        )
        await cg.register_component(var, conf)
        cg.add(var.set_radio(radio))
        cg.add(radio.set_buffer_capacity_number(var))
