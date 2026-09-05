# SPDX-License-Identifier: GPL-3.0-or-later
#
# Optional runtime control of the QoS used for the two RAM-buffered MQTT
# topics (raw telegram + /rx metadata - see ../mqtt_outbox.cpp). telegram_qos
# / rx_qos in the main wmbus_radio: block set the compiled starting value;
# these entities let that value be changed live between 0/1/2, e.g. from
# ESPHome's own web_server: + auth: - the same "lightweight authenticated
# portal" the buffer_capacity number: entity already uses instead of this
# component rolling its own HTTP server / auth handling.
#
# The C++ class lives in wmbus_qos_select.h next to this file - no explicit
# #include needed, see the note in ../number/__init__.py for why.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import CONF_RX_QOS, CONF_TELEGRAM_QOS, RadioComponent, radio_ns

DEPENDENCIES = ["wmbus_radio"]

CONF_WMBUS_RADIO_ID = "wmbus_radio_id"

WMBusQosSelect = radio_ns.class_("WMBusQosSelect", select.Select, cg.Component)
WMBusQosTarget = radio_ns.enum("WMBusQosTarget", is_class=True)

# Fixed set - QoS is only ever 0/1/2 in MQTT, matching the int_range(0, 2)
# already used for telegram_qos/rx_qos in the main component schema.
_QOS_OPTIONS = ["0", "1", "2"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_WMBUS_RADIO_ID): cv.use_id(RadioComponent),
        cv.Optional(CONF_TELEGRAM_QOS): select.select_schema(
            WMBusQosSelect,
            icon="mdi:transfer",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
        cv.Optional(CONF_RX_QOS): select.select_schema(
            WMBusQosSelect,
            icon="mdi:transfer",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)


async def _new_qos_select(conf, radio, target):
    var = await select.new_select(conf, options=_QOS_OPTIONS)
    await cg.register_component(var, conf)
    cg.add(var.set_radio(radio))
    cg.add(var.set_target(target))
    return var


async def to_code(config):
    radio = await cg.get_variable(config[CONF_WMBUS_RADIO_ID])

    if CONF_TELEGRAM_QOS in config:
        var = await _new_qos_select(config[CONF_TELEGRAM_QOS], radio, WMBusQosTarget.TELEGRAM)
        cg.add(radio.set_telegram_qos_select(var))

    if CONF_RX_QOS in config:
        var = await _new_qos_select(config[CONF_RX_QOS], radio, WMBusQosTarget.RX)
        cg.add(radio.set_rx_qos_select(var))
