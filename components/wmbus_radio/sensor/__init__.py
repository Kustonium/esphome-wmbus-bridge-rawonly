# SPDX-License-Identifier: GPL-3.0-or-later
#
# Optional visibility into the RAM MQTT outbox (see ../mqtt_outbox.cpp):
# how many frames are currently queued, how many have been dropped for lack
# of room, and how old the oldest still-queued frame is. All three are
# read-only gauges; declaring this platform is entirely optional and changes
# nothing about the buffer itself. Paired with the number: platform (buffer
# capacity) and ESPHome's own web_server: + auth:, this is the "lightweight
# authenticated portal to inspect the buffer" without wmbus_radio running its
# own HTTP server.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_SECOND,
)

from .. import RadioComponent

DEPENDENCIES = ["wmbus_radio"]

CONF_WMBUS_RADIO_ID = "wmbus_radio_id"
CONF_BUFFER_DEPTH = "buffer_depth"
CONF_BUFFER_DROPPED_TOTAL = "buffer_dropped_total"
CONF_BUFFER_OLDEST_PENDING_AGE = "buffer_oldest_pending_age"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_WMBUS_RADIO_ID): cv.use_id(RadioComponent),
        cv.Optional(CONF_BUFFER_DEPTH): sensor.sensor_schema(
            unit_of_measurement="frames",
            icon="mdi:tray-full",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_BUFFER_DROPPED_TOTAL): sensor.sensor_schema(
            unit_of_measurement="frames",
            icon="mdi:tray-remove",
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_BUFFER_OLDEST_PENDING_AGE): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            icon="mdi:clock-alert-outline",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    radio = await cg.get_variable(config[CONF_WMBUS_RADIO_ID])

    if CONF_BUFFER_DEPTH in config:
        sens = await sensor.new_sensor(config[CONF_BUFFER_DEPTH])
        cg.add(radio.set_buffer_depth_sensor(sens))

    if CONF_BUFFER_DROPPED_TOTAL in config:
        sens = await sensor.new_sensor(config[CONF_BUFFER_DROPPED_TOTAL])
        cg.add(radio.set_buffer_dropped_sensor(sens))

    if CONF_BUFFER_OLDEST_PENDING_AGE in config:
        sens = await sensor.new_sensor(config[CONF_BUFFER_OLDEST_PENDING_AGE])
        cg.add(radio.set_buffer_oldest_age_sensor(sens))
