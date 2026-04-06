from contextlib import suppress
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins, automation
from esphome.components import spi
from esphome.cpp_generator import LambdaExpression
from esphome.const import (
    CONF_ID,
    CONF_RESET_PIN,
    CONF_IRQ_PIN,
    CONF_TRIGGER_ID,
    CONF_FORMAT,
    CONF_DATA,
)
from pathlib import Path

CODEOWNERS = ["@SzczepanLeon", "@kubasaw", "@Kustonium"]

DEPENDENCIES = ["esp32", "spi", "mqtt"]

# Single public component only.
# Everything needed by the raw-only bridge lives inside wmbus_radio, so the user
# can keep a simple YAML declaration: components: [wmbus_radio]
AUTO_LOAD = []

MULTI_CONF = True

CONF_RADIO_ID = "radio_id"
CONF_ON_FRAME = "on_frame"
CONF_RADIO_TYPE = "radio_type"
CONF_MARK_AS_HANDLED = "mark_as_handled"
CONF_BUSY_PIN = "busy_pin"
CONF_LISTEN_MODE = "listen_mode"

# Optional built-in RAW forwarding (avoids YAML on_frame boilerplate)
CONF_TELEGRAM_TOPIC = "telegram_topic"
CONF_TARGET_METER_ID = "target_meter_id"
CONF_TARGET_TOPIC = "target_topic"
CONF_TARGET_LOG = "target_log"

# SX1262 board helpers
CONF_DIO2_RF_SWITCH = "dio2_rf_switch"
CONF_RF_SWITCH = "rf_switch"  # alias used by some configs
CONF_HAS_TCXO = "has_tcxo"

# RX gain option (datasheet: boosted / power_saving)
CONF_RX_GAIN = "rx_gain"
CONF_LONG_GFSK_PACKETS = "long_gfsk_packets"

# SX1262: clear latched device errors on boot (Semtech Get/ClearDeviceErrors)
CONF_CLEAR_DEVICE_ERRORS_ON_BOOT = "clear_device_errors_on_boot"
CONF_PUBLISH_DEV_ERR_AFTER_CLEAR = "publish_dev_err_after_clear"

# Log highlighting (optional)
CONF_HIGHLIGHT_METERS = "highlight_meters"
CONF_HIGHLIGHT_ANSI = "highlight_ansi"
CONF_HIGHLIGHT_TAG = "highlight_tag"
CONF_HIGHLIGHT_PREFIX = "highlight_prefix"

# Diagnostics
CONF_DIAG_TOPIC = "diagnostic_topic"
CONF_DIAG_VERBOSE = "diagnostic_verbose"
CONF_DIAG_PUBLISH_RAW = "diagnostic_publish_raw"
CONF_DIAG_SUMMARY_INTERVAL = "diagnostic_summary_interval"
CONF_DIAG_PUBLISH_SUMMARY_15MIN = "diagnostic_publish_summary_15min"
CONF_DIAG_PUBLISH_SUMMARY_60MIN = "diagnostic_publish_summary_60min"
CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS = "diagnostic_publish_summary_highlight_meters"
CONF_DIAG_PUBLISH_SUMMARY = "diagnostic_publish_summary"
CONF_DIAG_PUBLISH_DROP_EVENTS = "diagnostic_publish_drop_events"
CONF_DIAG_PUBLISH_RX_PATH_EVENTS = "diagnostic_publish_rx_path_events"
CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY = "diagnostic_publish_highlight_only"
CONF_SX1276_BUSY_ETHER_MODE = "sx1276_busy_ether_mode"

# Heltec V4 FEM pins (SX1262 external front-end)
CONF_FEM_CTRL_PIN = "fem_ctrl_pin"
CONF_FEM_EN_PIN = "fem_en_pin"
CONF_FEM_PA_PIN = "fem_pa_pin"

radio_ns = cg.esphome_ns.namespace("wmbus_radio")
RadioComponent = radio_ns.class_("Radio", cg.Component)
RadioTransceiver = radio_ns.class_("RadioTransceiver", spi.SPIDevice, cg.Component)
Frame = radio_ns.class_("Frame")
FrameOutputFormat = Frame.enum("OutputFormat")
FramePtr = Frame.operator("ptr")
FrameTrigger = radio_ns.class_("FrameTrigger", automation.Trigger.template(FramePtr))

TRANSCEIVER_NAMES = {
    r.stem.removeprefix("transceiver_").upper()
    for r in Path(__file__).parent.glob("transceiver_*.cpp")
    if r.is_file()
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RadioComponent),
            cv.GenerateID(CONF_RADIO_ID): cv.declare_id(RadioTransceiver),
            cv.Required(CONF_RADIO_TYPE): cv.one_of(*TRANSCEIVER_NAMES, upper=True),
            cv.Required(CONF_RESET_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_IRQ_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_BUSY_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_LISTEN_MODE, default="both"): cv.one_of(
                "t1", "c1", "both", lower=True
            ),

            # SX1262-specific tuning (ignored for other radios)
            cv.Optional(CONF_DIO2_RF_SWITCH, default=True): cv.boolean,
            cv.Optional(CONF_RF_SWITCH): cv.boolean,
            cv.Optional(CONF_HAS_TCXO, default=False): cv.boolean,
            cv.Optional(CONF_RX_GAIN, default="boosted"): cv.one_of(
                "boosted", "power_saving", lower=True
            ),
            cv.Optional(CONF_LONG_GFSK_PACKETS, default=False): cv.boolean,

            # Heltec V4 FEM pins (optional, only makes sense for SX1262)
            cv.Optional(CONF_FEM_CTRL_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM_EN_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM_PA_PIN): pins.internal_gpio_output_pin_schema,

            cv.Optional(CONF_ON_FRAME): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FrameTrigger),
                    cv.Optional(CONF_MARK_AS_HANDLED, default=False): cv.boolean,
                }
            ),


            # Optional built-in RAW forwarding (publish full frame hex and/or one selected target meter)
            cv.Optional(CONF_TELEGRAM_TOPIC, default=""): cv.string,
            cv.Optional(CONF_TARGET_METER_ID, default=""): cv.string,
            cv.Optional(CONF_TARGET_TOPIC, default=""): cv.string,
            cv.Optional(CONF_TARGET_LOG, default=True): cv.boolean,

            # Publish diagnostics (e.g. truncated frames) to MQTT
            cv.Optional(CONF_DIAG_TOPIC, default="wmbus/diag"): cv.string,

            # Diagnostics verbosity / publication controls
            cv.Optional(CONF_DIAG_VERBOSE, default=True): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_RAW, default=True): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY, default=True): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_DROP_EVENTS, default=True): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_RX_PATH_EVENTS, default=True): cv.boolean,
            # If true, per-packet MQTT diagnostics are published only for meter ids
            # listed in highlight_meters. Global summary still counts everything.
            cv.Optional(CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_DIAG_SUMMARY_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY_15MIN, default=False): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY_60MIN, default=False): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS, default=False): cv.boolean,
            cv.Optional(CONF_SX1276_BUSY_ETHER_MODE, default="adaptive"): cv.one_of(
                "normal", "aggressive", "adaptive", lower=True
            ),

            # Optional log highlighting for selected meter IDs
            cv.Optional(CONF_HIGHLIGHT_METERS, default=[]): cv.ensure_list(cv.string),
            cv.Optional(CONF_HIGHLIGHT_ANSI, default=False): cv.boolean,
            cv.Optional(CONF_HIGHLIGHT_TAG, default="wmbus_user"): cv.string,
            cv.Optional(CONF_HIGHLIGHT_PREFIX, default="★ "): cv.string,

            # SX1262: device errors handling on boot
            cv.Optional(CONF_CLEAR_DEVICE_ERRORS_ON_BOOT, default=False): cv.boolean,
            cv.Optional(CONF_PUBLISH_DEV_ERR_AFTER_CLEAR, default=False): cv.boolean,
        }
    )
    .extend(spi.spi_device_schema())
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    cg.add(cg.LineComment("WMBus RadioTransceiver"))

    config[CONF_RADIO_ID].type = radio_ns.class_(
        config[CONF_RADIO_TYPE], RadioTransceiver
    )
    radio_var = cg.new_Pvariable(config[CONF_RADIO_ID])

    if config[CONF_RADIO_TYPE] == "SX1262":
        dio2_rf = config.get(CONF_RF_SWITCH, config.get(CONF_DIO2_RF_SWITCH, True))
        cg.add(radio_var.set_dio2_rf_switch(dio2_rf))
        cg.add(radio_var.set_has_tcxo(config.get(CONF_HAS_TCXO, False)))

        SX1262RxGain = radio_ns.enum("SX1262RxGain")
        gain = config.get(CONF_RX_GAIN, "boosted")
        cg.add(
            radio_var.set_rx_gain(
                SX1262RxGain.BOOSTED
                if gain == "boosted"
                else SX1262RxGain.POWER_SAVING
            )
        )
        cg.add(radio_var.set_long_gfsk_packets(config.get(CONF_LONG_GFSK_PACKETS, False)))

        # Clear SX1262 device errors on boot (optional)
        cg.add(radio_var.set_clear_device_errors_on_boot(config.get(CONF_CLEAR_DEVICE_ERRORS_ON_BOOT, False)))

        # FEM pins (Heltec V4)
        if CONF_FEM_CTRL_PIN in config:
            p = await cg.gpio_pin_expression(config[CONF_FEM_CTRL_PIN])
            cg.add(radio_var.set_fem_ctrl_pin(p))
        if CONF_FEM_EN_PIN in config:
            p = await cg.gpio_pin_expression(config[CONF_FEM_EN_PIN])
            cg.add(radio_var.set_fem_en_pin(p))
        if CONF_FEM_PA_PIN in config:
            p = await cg.gpio_pin_expression(config[CONF_FEM_PA_PIN])
            cg.add(radio_var.set_fem_pa_pin(p))

    reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(radio_var.set_reset_pin(reset_pin))

    ListenMode = radio_ns.enum("ListenMode", is_class=False)
    listen_mode_map = {
        "t1": ListenMode.LISTEN_MODE_T1,
        "c1": ListenMode.LISTEN_MODE_C1,
        "both": ListenMode.LISTEN_MODE_BOTH,
    }
    cg.add(radio_var.set_listen_mode(listen_mode_map[config[CONF_LISTEN_MODE]]))

    irq_pin = await cg.gpio_pin_expression(config[CONF_IRQ_PIN])
    cg.add(radio_var.set_irq_pin(irq_pin))

    if CONF_BUSY_PIN in config:
        busy_pin = await cg.gpio_pin_expression(config[CONF_BUSY_PIN])
        cg.add(radio_var.set_busy_pin(busy_pin))

    await spi.register_spi_device(radio_var, config)
    await cg.register_component(radio_var, config)

    cg.add(cg.LineComment("WMBus Component"))
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_radio(radio_var))

    cg.add(var.set_diag_topic(config.get(CONF_DIAG_TOPIC, "wmbus/diag")))
    cg.add(var.set_telegram_topic(config.get(CONF_TELEGRAM_TOPIC, "")))
    cg.add(var.set_target_meter_id_str(config.get(CONF_TARGET_METER_ID, "")))
    cg.add(var.set_target_topic(config.get(CONF_TARGET_TOPIC, "")))
    cg.add(var.set_target_log(config.get(CONF_TARGET_LOG, True)))

    cg.add(var.set_diag_verbose(config.get(CONF_DIAG_VERBOSE, True)))
    cg.add(var.set_diag_publish_raw(config.get(CONF_DIAG_PUBLISH_RAW, True)))
    cg.add(var.set_diag_publish_summary(config.get(CONF_DIAG_PUBLISH_SUMMARY, True)))
    cg.add(var.set_diag_publish_drop_events(config.get(CONF_DIAG_PUBLISH_DROP_EVENTS, True)))
    cg.add(var.set_diag_publish_rx_path_events(config.get(CONF_DIAG_PUBLISH_RX_PATH_EVENTS, True)))
    cg.add(var.set_diag_publish_highlight_only(config.get(CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY, False)))
    cg.add(var.set_diag_summary_interval_ms(config[CONF_DIAG_SUMMARY_INTERVAL].total_milliseconds))
    cg.add(var.set_diag_publish_summary_15min(config.get(CONF_DIAG_PUBLISH_SUMMARY_15MIN, False)))
    cg.add(var.set_diag_publish_summary_60min(config.get(CONF_DIAG_PUBLISH_SUMMARY_60MIN, False)))
    cg.add(var.set_diag_publish_summary_highlight_meters(config.get(CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS, False)))

    SX1276BusyEtherMode = radio_ns.enum("SX1276BusyEtherMode", is_class=True)
    busy_ether_mode_map = {
        "normal": SX1276BusyEtherMode.NORMAL,
        "aggressive": SX1276BusyEtherMode.AGGRESSIVE,
        "adaptive": SX1276BusyEtherMode.ADAPTIVE,
    }
    cg.add(var.set_sx1276_busy_ether_mode(busy_ether_mode_map[config.get(CONF_SX1276_BUSY_ETHER_MODE, "adaptive")]))

    # Log highlight config
    meters = config.get(CONF_HIGHLIGHT_METERS, [])
    meters_csv = ",".join([str(m).strip() for m in meters if str(m).strip()])
    cg.add(var.set_highlight_meters_csv(meters_csv))
    cg.add(var.set_highlight_ansi(config.get(CONF_HIGHLIGHT_ANSI, False)))
    cg.add(var.set_highlight_tag(config.get(CONF_HIGHLIGHT_TAG, "wmbus_user")))
    cg.add(var.set_highlight_prefix(config.get(CONF_HIGHLIGHT_PREFIX, "★ ")))

    # Optional: publish SX1262 dev_err before/after clear (once after boot)
    cg.add(var.set_publish_dev_err_after_clear(config.get(CONF_PUBLISH_DEV_ERR_AFTER_CLEAR, False)))

    await cg.register_component(var, config)

    for conf in config.get(CONF_ON_FRAME, []):
        trig = cg.new_Pvariable(
            conf[CONF_TRIGGER_ID], var, conf[CONF_MARK_AS_HANDLED]
        )
        await automation.build_automation(
            trig,
            [(FramePtr, "frame")],
            conf,
        )


with suppress(ImportError):
    from ..socket_transmitter import (
        SOCKET_SEND_ACTION_SCHEMA,
        SocketTransmitterSendAction,
    )

    FRAME_SOCKET_SEND_SCHEMA = SOCKET_SEND_ACTION_SCHEMA.extend(
        {
            cv.Required(CONF_FORMAT): cv.one_of(
                "hex",
                "raw",
                "rtlwmbus",
                lower=True,
            ),
            cv.Optional(CONF_DATA): cv.invalid(
                "If you want to specify data to be sent, use generic 'socket_transmitter.send' action"
            ),
        }
    )

    @automation.register_action(
        "wmbus_radio.send_frame_with_socket",
        SocketTransmitterSendAction,
        FRAME_SOCKET_SEND_SCHEMA,
    )
    async def send_frame_with_socket_to_code(config, action_id, template_arg, args):
        output_type = {
            "hex": cg.std_string,
            "raw": cg.std_vector.template(cg.uint8),
            "rtlwmbus": cg.std_string,
        }[config[CONF_FORMAT]]

        paren = await cg.get_variable(config[CONF_ID])
        var = cg.new_Pvariable(
            action_id, cg.TemplateArguments(output_type, *template_arg), paren
        )
        template_ = LambdaExpression(
            f"return frame.as_{config[CONF_FORMAT]}();", args, ""
        )

        cg.add(var.set_data(template_))

        return var
