# SPDX-License-Identifier: GPL-3.0-or-later
from contextlib import suppress
import logging
import re
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins, automation
from esphome.components import spi
from esphome.cpp_generator import LambdaExpression
from esphome.core import CORE
from esphome.const import (
    CONF_ID,
    CONF_RESET_PIN,
    CONF_IRQ_PIN,
    CONF_TRIGGER_ID,
    CONF_FORMAT,
    CONF_DATA,
)
from pathlib import Path

_LOGGER = logging.getLogger(__name__)

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
CONF_LISTEN_MODE_FILTER_AFTER_PARSE = "listen_mode_filter_after_parse"
CONF_RECEIVER_TASK_STACK_SIZE = "receiver_task_stack_size"

# Optional built-in RAW forwarding (avoids YAML on_frame boilerplate)
CONF_TOPIC_NAME = "topic_name"
CONF_TELEGRAM_TOPIC = "telegram_topic"
CONF_TARGET_METER_ID = "target_meter_id"
CONF_TARGET_TOPIC = "target_topic"
CONF_TARGET_LOG = "target_log"
CONF_PUBLISH_RADIO_RAW = "publish_radio_raw"
CONF_PUBLISH_RSSI = "publish_rssi"
CONF_FORWARD_METERS = "forward_meters"

# SX1262 board helpers
CONF_DIO2_RF_SWITCH = "dio2_rf_switch"
CONF_RF_SWITCH = "rf_switch"  # alias used by some configs
CONF_HAS_TCXO = "has_tcxo"

# SX1276 board helper: optional external TCXO enable/power pin.
CONF_TCXO_PIN = "tcxo_pin"

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
CONF_DIAGNOSTIC_MODE = "diagnostic_mode"
CONF_DIAG_VERBOSE = "diagnostic_verbose"
CONF_DIAG_PUBLISH_RAW = "diagnostic_publish_raw"
CONF_DIAG_SUMMARY_INTERVAL = "diagnostic_summary_interval"
CONF_DIAG_PUBLISH_SUMMARY_15MIN = "diagnostic_publish_summary_15min"
CONF_DIAG_PUBLISH_SUMMARY_60MIN = "diagnostic_publish_summary_60min"
CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS = "diagnostic_publish_summary_highlight_meters"
CONF_DIAG_PUBLISH_SUMMARY = "diagnostic_publish_summary"
CONF_DIAG_PUBLISH_DROP_EVENTS = "diagnostic_publish_drop_events"
CONF_DIAG_PUBLISH_RX_PATH_EVENTS = "diagnostic_publish_rx_path_events"
CONF_DIAG_EVENTS_HIGHLIGHT_ONLY = "diagnostic_events_highlight_only"
CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY = "diagnostic_publish_highlight_only"
CONF_DIAG_METER_STATS = "diagnostic_meter_stats"
CONF_DIAG_PUBLISH_SUGGESTION = "diagnostic_publish_suggestion"
CONF_SX1276_BUSY_ETHER_MODE = "sx1276_busy_ether_mode"

# Heltec V4 FEM pins (SX1262 external front-end)
CONF_FEM_CTRL_PIN = "fem_ctrl_pin"
CONF_FEM_EN_PIN = "fem_en_pin"
CONF_FEM_PA_PIN = "fem_pa_pin"

# External gate of the module's internal RF switch (SX1262).
# Needed by modules that do not connect the antenna unconditionally, such as the
# Seeed Wio-SX1262 (module pin 1 RF_SW; GPIO38 on the XIAO ESP32S3 kit). Not the
# same thing as dio2_rf_switch, which only selects the TX/RX direction.
CONF_RF_SW_PIN = "rf_sw_pin"

# CC1101 pins / safety gate (experimental, advanced users only)
CONF_GDO0_PIN = "gdo0_pin"
CONF_GDO2_PIN = "gdo2_pin"
CONF_CC1101_ALLOW_EXPERIMENTAL = "cc1101_allow_experimental"
CONF_ALLOW_UNTESTED_FRAMEWORK = "allow_untested_framework"
CONF_FREQUENCY = "frequency"

# LR1121 (Waveshare ESP32-S3-LR1121-XF). Experimental in the strong sense:
# written from datasheet + schematic + vendor package, never run against
# hardware. Gated the same way CC1101 is.
CONF_LR1121_ALLOW_EXPERIMENTAL = "lr1121_allow_experimental"
CONF_TCXO_VOLTAGE = "tcxo_voltage"
# Second TCXO knob. HF_XOSC_START does not distinguish "wrong voltage" from
# "did not settle in time", so both have to be reachable from YAML.
# 300 RTC ticks at 32.768 kHz is ~9.2 ms, the vendor value.
CONF_TCXO_STARTUP_TICKS = "tcxo_startup_ticks"
CONF_RX_BANDWIDTH = "rx_bandwidth"
CONF_PREAMBLE_DETECTOR = "preamble_detector"
CONF_PAYLOAD_LENGTH = "payload_length"
CONF_RX_BOOSTED = "rx_boosted"
CONF_BITRATE = "bitrate"
CONF_DEVIATION = "deviation"

# YAML value -> C++ enumerator suffix. Kept as plain maps so the schema and the
# code generator cannot drift apart.
LR1121_TCXO_VOLTAGES = {
    "1.6v": "LR1121_TCXO_1_6V",
    "1.7v": "LR1121_TCXO_1_7V",
    "1.8v": "LR1121_TCXO_1_8V",
    "2.2v": "LR1121_TCXO_2_2V",
    "2.4v": "LR1121_TCXO_2_4V",
    "2.7v": "LR1121_TCXO_2_7V",
    "3.0v": "LR1121_TCXO_3_0V",
    "3.3v": "LR1121_TCXO_3_3V",
}

LR1121_RX_BANDWIDTHS = {
    "234300": "LR1121_BW_234300",
    "312000": "LR1121_BW_312000",
    "373600": "LR1121_BW_373600",
    "467000": "LR1121_BW_467000",
}

LR1121_PREAMBLE_DETECTORS = {
    "off": "LR1121_PREAMBLE_OFF",
    "8": "LR1121_PREAMBLE_MIN_8B",
    "16": "LR1121_PREAMBLE_MIN_16B",
    "24": "LR1121_PREAMBLE_MIN_24B",
    "32": "LR1121_PREAMBLE_MIN_32B",
}

# Schema defaults, and the SINGLE SOURCE for them: the cv.Optional entries below
# read their default straight out of this dict.
#
# They used to be written twice. On 2026-08-19 the schema was updated to the
# measured values (3.0v / 3000 / 255) and this dict was not, which broke every
# non-LR1121 board: the validator compares a config value against this dict to
# decide "did the user set an LR1121 option on the wrong radio", and a
# schema-injected default that disagrees with the dict looks exactly like a user
# setting it. CC1101, SX1262 and SX1276 configs were all rejected with a message
# about tcxo_voltage. Keeping one copy makes that class of bug impossible.
BASE_CONFIG_DEFAULTS_LR1121 = {
    CONF_TCXO_VOLTAGE: "3.0v",
    CONF_TCXO_STARTUP_TICKS: 3000,
    CONF_RX_BANDWIDTH: "234300",
    CONF_PREAMBLE_DETECTOR: "16",
    CONF_PAYLOAD_LENGTH: 255,
    CONF_RX_BOOSTED: True,
    CONF_BITRATE: 100000,
    CONF_DEVIATION: 50000,
}

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


def _validate_topic_name(value):
    value = str(value).strip()
    if not value:
        raise cv.Invalid("topic_name cannot be empty / topic_name nie moze byc pusty")
    if value.startswith("wmbus/") or "/" in value:
        raise cv.Invalid("topic_name must not contain '/' or 'wmbus/' prefix / topic_name nie moze zawierac '/' ani prefiksu 'wmbus/'")
    if "+" in value or "#" in value:
        raise cv.Invalid("topic_name must not contain MQTT wildcards '+' or '#' / topic_name nie moze zawierac wildcardow MQTT '+' ani '#'")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise cv.Invalid("topic_name may only contain letters, numbers, '_' and '-' / topic_name moze zawierac tylko litery, cyfry, '_' i '-'")
    return value


_METER_ID_MAX_BCD = 99999999


def _validate_meter_id(value):
    """One meter ID: decimal for a BCD meter, hex for a meter whose A-field is not BCD.

    The trap this guards against: YAML resolves an unquoted 0x417F0666 to the
    integer 1098843750, which would silently be stored as a decimal ID and could
    never match anything. Any all-digit value above the largest possible BCD ID
    is therefore rejected, with the quoted hex form spelled out.
    """
    text = str(value).strip()
    if not text:
        raise cv.Invalid("meter ID cannot be empty / ID licznika nie moze byc puste")

    body = text[2:] if text[:2].lower() == "0x" else text
    if not body:
        raise cv.Invalid(f"'{text}' is missing its hex digits / brakuje cyfr szesnastkowych")

    is_hex = text[:2].lower() == "0x" or any(c in "abcdefABCDEF" for c in body)

    if is_hex:
        try:
            raw = int(body, 16)
        except ValueError:
            raise cv.Invalid(
                f"'{text}' is not a valid meter ID - use the value the log prints as id:, "
                f"decimal (e.g. 12345678) or hex (e.g. \"0x417F0666\") / "
                f"uzyj wartosci, ktora log wypisuje jako id:"
            )
        if raw > 0xFFFFFFFF:
            raise cv.Invalid(f"'{text}' does not fit in a 4-byte meter ID / nie miesci sie w 4 bajtach")
        return text

    if not body.isdigit():
        raise cv.Invalid(
            f"'{text}' is not a valid meter ID - use decimal (e.g. 12345678) or hex "
            f'(e.g. "0x417F0666") / uzyj zapisu dziesietnego albo szesnastkowego'
        )

    if int(body) > _METER_ID_MAX_BCD:
        raise cv.Invalid(
            f"meter ID {body} exceeds the largest possible BCD meter ID ({_METER_ID_MAX_BCD}). "
            f'If this is the hex ID from the log, quote it so YAML keeps it as text: "0x{int(body):08X}" / '
            f'Jesli to szesnastkowe ID z logu, ujmij je w cudzyslow: "0x{int(body):08X}"'
        )

    return text


def _meters_csv(values):
    """Join a YAML meter-ID list into the CSV string the C++ side parses."""
    return ",".join([str(m).strip() for m in values if str(m).strip()])


def _normalize_diagnostic_mode(mode):
    mode = str(mode).lower().strip()
    if mode == "medium":
        return "normal"
    if mode in ("full", "raw"):
        return "dev"
    return mode

BASE_CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RadioComponent),
            cv.GenerateID(CONF_RADIO_ID): cv.declare_id(RadioTransceiver),
            cv.Required(CONF_RADIO_TYPE): cv.one_of(*TRANSCEIVER_NAMES, upper=True),
            # SX1262/SX1276 use reset_pin + irq_pin.
            # CC1101 intentionally uses gdo0_pin + gdo2_pin instead.
            cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_IRQ_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_BUSY_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_GDO0_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_GDO2_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_CC1101_ALLOW_EXPERIMENTAL, default=False): cv.boolean,
            cv.Optional(CONF_ALLOW_UNTESTED_FRAMEWORK, default=False): cv.boolean,
            cv.Optional(CONF_FREQUENCY): cv.float_range(min=300.0, max=928.0),
            cv.Optional(CONF_LISTEN_MODE, default="both"): cv.one_of(
                "t1", "c1", "s1", "both", lower=True
            ),
            # Advanced/experimental. Default false keeps the legacy behavior:
            # filter listen_mode by preliminary raw packet mode before parsing.
            # True tries parser/CRC-selected mode first, then filters afterwards.
            cv.Optional(CONF_LISTEN_MODE_FILTER_AFTER_PARSE, default=False): cv.boolean,
            # Stack size for the dedicated radio_recv FreeRTOS task created by this
            # component. This is intentionally separate from ESPHome's
            # loop_task_stack_size because that YAML option only affects the main
            # loop task, while wmbus_radio uses its own receiver task.
            #
            # Why this exists: some smaller / different boards (for example XIAO)
            # can be fine on 1.0.x and then overflow the receiver task stack on a
            # newer build with heavier diagnostics enabled. Making it configurable
            # avoids per-board branches and keeps one shared codebase.
            cv.Optional(CONF_RECEIVER_TASK_STACK_SIZE, default=3072): cv.int_range(min=2048, max=16384),

            # SX1262-specific tuning (ignored for other radios)
            cv.Optional(CONF_DIO2_RF_SWITCH, default=True): cv.boolean,
            cv.Optional(CONF_RF_SWITCH): cv.boolean,
            cv.Optional(CONF_HAS_TCXO, default=False): cv.boolean,
            cv.Optional(CONF_RX_GAIN, default="boosted"): cv.one_of(
                "boosted", "power_saving", lower=True
            ),
            cv.Optional(CONF_LONG_GFSK_PACKETS, default=False): cv.boolean,

            # SX1276-specific board helper (for boards such as LilyGO T3 V3.0 TCXO).
            cv.Optional(CONF_TCXO_PIN): pins.internal_gpio_output_pin_schema,

            # LR1121-specific tuning (ignored for other radios).
            #
            # Everything the RF path depends on is exposed here on purpose. The
            # numbers this project measures come from one dense block of flats
            # with ~29 meters; they are a good default and a bad law. Freezing
            # them into the driver would repeat exactly the criticism this
            # project makes of hardcoded bandwidths elsewhere.
            cv.Optional(CONF_LR1121_ALLOW_EXPERIMENTAL, default=False): cv.boolean,
            cv.Optional(CONF_TCXO_VOLTAGE, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_TCXO_VOLTAGE]): cv.one_of(
                *LR1121_TCXO_VOLTAGES, lower=True
            ),
            cv.Optional(CONF_TCXO_STARTUP_TICKS, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_TCXO_STARTUP_TICKS]): cv.int_range(min=1, max=16777215),
            cv.Optional(CONF_RX_BANDWIDTH, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_RX_BANDWIDTH]): cv.one_of(
                *LR1121_RX_BANDWIDTHS, lower=True
            ),
            cv.Optional(CONF_PREAMBLE_DETECTOR, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_PREAMBLE_DETECTOR]): cv.one_of(
                *LR1121_PREAMBLE_DETECTORS, lower=True
            ),
            cv.Optional(CONF_PAYLOAD_LENGTH, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_PAYLOAD_LENGTH]): cv.int_range(min=16, max=255),
            cv.Optional(CONF_RX_BOOSTED, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_RX_BOOSTED]): cv.boolean,
            cv.Optional(CONF_BITRATE, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_BITRATE]): cv.int_range(min=600, max=300000),
            cv.Optional(CONF_DEVIATION, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_DEVIATION]): cv.int_range(min=1000, max=200000),

            # Heltec V4 FEM pins (optional, only makes sense for SX1262)
            cv.Optional(CONF_FEM_CTRL_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM_EN_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM_PA_PIN): pins.internal_gpio_output_pin_schema,
            # External RF switch gate (optional, only makes sense for SX1262)
            cv.Optional(CONF_RF_SW_PIN): pins.internal_gpio_output_pin_schema,

            cv.Optional(CONF_ON_FRAME): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FrameTrigger),
                    cv.Optional(CONF_MARK_AS_HANDLED, default=False): cv.boolean,
                }
            ),


            # Optional built-in RAW forwarding (publish full frame hex and/or one selected target meter)
            cv.Optional(CONF_TOPIC_NAME): _validate_topic_name,
            # Legacy/manual override. Prefer topic_name.
            cv.Optional(CONF_TELEGRAM_TOPIC): cv.string,
            cv.Optional(CONF_TARGET_METER_ID, default=""): cv.string,
            cv.Optional(CONF_TARGET_TOPIC, default=""): cv.string,
            cv.Optional(CONF_TARGET_LOG, default=True): cv.boolean,
            # Internal/dev-only raw packet tap. Fixed MQTT topic: wmbus_bridge/raw.
            cv.Optional(CONF_PUBLISH_RADIO_RAW, default=False): cv.boolean,
            # Publish the measured RSSI of each forwarded meter frame to
            # wmbus/<topic_name>/rssi/<meter_id>. Disabled by default.
            cv.Optional(CONF_PUBLISH_RSSI, default=False): cv.boolean,
            # Whitelist of meter IDs allowed on the RAW telegram topic. Accepts
            # either an explicit list, or `true` to reuse highlight_meters so the
            # same IDs are not written twice. Empty (default) forwards every
            # decoded frame, as before this option existed.
            cv.Optional(CONF_FORWARD_METERS, default=[]): cv.Any(
                cv.boolean, cv.ensure_list(_validate_meter_id)
            ),

            # Diagnostics are opt-in by default. `diagnostic_mode` applies a preset
            # for MQTT publishing only; explicit detailed flags still override it.
            cv.Optional(CONF_DIAGNOSTIC_MODE, default="off"): cv.one_of(
                "off", "low", "normal", "debug", "dev", "medium", "full", "raw", lower=True
            ),
            # Legacy/manual override. Prefer topic_name.
            cv.Optional(CONF_DIAG_TOPIC): cv.string,

            # Detailed diagnostics controls (all optional; explicit YAML values override
            # the preset selected by diagnostic_mode).
            cv.Optional(CONF_DIAG_VERBOSE): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_RAW): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_DROP_EVENTS): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_RX_PATH_EVENTS): cv.boolean,
            # If true, per-packet MQTT diagnostics are published only for meter ids
            # listed in highlight_meters. Global summary still counts everything.
            cv.Optional(CONF_DIAG_EVENTS_HIGHLIGHT_ONLY): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY): cv.boolean,
            cv.Optional(CONF_DIAG_METER_STATS): cv.one_of("off", "highlighted", "all", lower=True),
            cv.Optional(CONF_DIAG_PUBLISH_SUGGESTION): cv.boolean,
            cv.Optional(CONF_DIAG_SUMMARY_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY_15MIN): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY_60MIN): cv.boolean,
            cv.Optional(CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS): cv.boolean,
            cv.Optional(CONF_SX1276_BUSY_ETHER_MODE, default="normal"): cv.one_of(
                "normal", "aggressive", "adaptive", lower=True
            ),

            # Optional log highlighting for selected meter IDs
            cv.Optional(CONF_HIGHLIGHT_METERS, default=[]): cv.ensure_list(_validate_meter_id),
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


# ─────────────────────────────────────────────────────────────────────────────
# Boot configuration report
#
# The startup log used to show a handful of hand-picked "sanity" lines, so any
# option outside that list was invisible: a board could be misconfigured and
# the log looked healthy. Worse, a reader could not tell whether a value was
# chosen or simply inherited - which is the difference between "I set rx_gain"
# and "rx_gain happens to be boosted".
#
# The report is built HERE, not in C++, because this is the only place that
# knows both the user's config and the schema defaults. Duplicating defaults in
# the driver is exactly the drift that tests/ci/check_example_defaults.py exists
# to prevent.
# ─────────────────────────────────────────────────────────────────────────────
def _schema_defaults():
    """Map option name -> schema default. Cosmetic; never break a build over it."""
    out = {}
    try:
        import voluptuous as vol

        for key in BASE_CONFIG_SCHEMA.schema:
            name = getattr(key, "schema", None)
            if not isinstance(name, str):
                continue
            default = getattr(key, "default", None)
            if default is None:
                continue
            value = default() if callable(default) else default
            if value is vol.UNDEFINED:
                continue
            out[name] = value
    except Exception:  # noqa: BLE001
        return {}
    return out


def _report_value(value, key=None):
    # forward_meters accepts a list or a bare false, and both mean the same
    # thing: no whitelist. Rendering them differently produced a "(CHANGED)"
    # on a setting that changes nothing.
    if key == CONF_FORWARD_METERS and (value is False or (isinstance(value, (list, tuple)) and not value)):
        return "disabled (no whitelist)"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, dict) and "number" in value:
        return f"GPIO{value['number']}"
    if isinstance(value, (list, tuple)):
        return f"{len(value)} entries" if value else "empty"
    if hasattr(value, "total_milliseconds"):
        return f"{value.total_milliseconds / 1000:g}s"
    if value == "":
        return '""'
    return str(value)


_REPORT_PINS = ("cs_pin", CONF_RESET_PIN, CONF_IRQ_PIN, CONF_BUSY_PIN,
                CONF_GDO0_PIN, CONF_GDO2_PIN, CONF_TCXO_PIN, CONF_RF_SW_PIN,
                CONF_FEM_EN_PIN, CONF_FEM_CTRL_PIN, CONF_FEM_PA_PIN)

_REPORT_CORE = (CONF_RADIO_TYPE, CONF_LISTEN_MODE, CONF_LISTEN_MODE_FILTER_AFTER_PARSE,
                CONF_FREQUENCY, CONF_RECEIVER_TASK_STACK_SIZE, CONF_ALLOW_UNTESTED_FRAMEWORK)

_REPORT_RADIO = {
    "SX1262": (CONF_HAS_TCXO, CONF_DIO2_RF_SWITCH, CONF_RF_SWITCH, CONF_RX_GAIN,
               CONF_LONG_GFSK_PACKETS, CONF_CLEAR_DEVICE_ERRORS_ON_BOOT,
               CONF_PUBLISH_DEV_ERR_AFTER_CLEAR),
    "SX1276": (CONF_SX1276_BUSY_ETHER_MODE,),
    "CC1101": (CONF_CC1101_ALLOW_EXPERIMENTAL,),
    "LR1121": (CONF_LR1121_ALLOW_EXPERIMENTAL, CONF_TCXO_VOLTAGE, CONF_TCXO_STARTUP_TICKS,
               CONF_RX_BANDWIDTH, CONF_PREAMBLE_DETECTOR, CONF_PAYLOAD_LENGTH,
               CONF_RX_BOOSTED, CONF_BITRATE, CONF_DEVIATION),
}

_REPORT_OUTPUT = (CONF_TOPIC_NAME, CONF_TELEGRAM_TOPIC, CONF_PUBLISH_RSSI,
                  CONF_FORWARD_METERS, CONF_TARGET_METER_ID, CONF_PUBLISH_RADIO_RAW)

_REPORT_DIAG = (CONF_DIAGNOSTIC_MODE, CONF_DIAG_SUMMARY_INTERVAL, CONF_DIAG_TOPIC,
                CONF_HIGHLIGHT_METERS, CONF_DIAG_METER_STATS, CONF_DIAG_VERBOSE)


def _config_report_lines(config):
    """Every effective setting, marked (default) or (changed)."""
    defaults = _schema_defaults()
    radio_type = str(config[CONF_RADIO_TYPE]).upper()
    lines = []

    def emit(key):
        if key == CONF_RADIO_TYPE:
            lines.append(f"  {key}: {config[key]} (required)")
            return
        if key not in config:
            if key == CONF_FREQUENCY:
                lines.append("  frequency: not set (mode default: 868.950 MHz for t1/c1/both, 868.300 MHz for s1)")
            elif key in defaults:
                lines.append(f"  {key}: not set (default: {_report_value(defaults[key], key)})")
            else:
                lines.append(f"  {key}: not set")
            return
        value = _report_value(config[key], key)
        if key not in defaults:
            lines.append(f"  {key}: {value} (set)")
            return
        # Compare the RENDERED values, not the raw ones. A schema default is
        # written as the user would write it ("60s"), while the validated config
        # holds the parsed object - so raw equality reported a change on a
        # setting nobody touched.
        default_value = _report_value(defaults[key], key)
        if value == default_value:
            lines.append(f"  {key}: {value} (default)")
        else:
            lines.append(f"  {key}: {value} (CHANGED, default: {default_value})")

    for title, keys in (
        ("core", _REPORT_CORE),
        ("pins", _REPORT_PINS),
        (f"{radio_type.lower()}", _REPORT_RADIO.get(radio_type, ())),
        ("output", _REPORT_OUTPUT),
        ("diagnostics", _REPORT_DIAG),
    ):
        reported = [k for k in keys if k in config or k in defaults or k == CONF_FREQUENCY]
        if title == "pins":
            # A pin that does not exist on this radio is noise, not information.
            reported = [k for k in keys if k in config]
        if not reported:
            continue
        lines.append(f"  [{title}]")
        for key in reported:
            emit(key)
    return lines


def _validate_radio_pins(config):
    radio_type = config[CONF_RADIO_TYPE].upper()

    if CONF_TCXO_PIN in config and radio_type != "SX1276":
        raise cv.Invalid("tcxo_pin is only valid for radio_type: SX1276. For SX1262 use has_tcxo instead.")

    if radio_type == "CC1101":
        if not config.get(CONF_CC1101_ALLOW_EXPERIMENTAL, False):
            raise cv.Invalid(
                "CC1101 support is experimental. Set cc1101_allow_experimental: true after reading the documentation. "
                "CC1101 requires validated wiring and both GDO0+GDO2 pins."
            )
        if CONF_GDO0_PIN not in config:
            raise cv.Invalid("CC1101 requires gdo0_pin (FIFO/data event).")
        if CONF_GDO2_PIN not in config:
            raise cv.Invalid("CC1101 requires gdo2_pin (sync detection). Single-IRQ CC1101 wiring is not supported.")
        if CONF_IRQ_PIN in config:
            raise cv.Invalid("For CC1101 use gdo0_pin and gdo2_pin, not irq_pin.")
        if CONF_RESET_PIN in config:
            raise cv.Invalid("CC1101 does not use reset_pin in this component. Remove reset_pin.")
        if CONF_BUSY_PIN in config:
            raise cv.Invalid("CC1101 does not use busy_pin. Remove busy_pin.")
    else:
        if CONF_RESET_PIN not in config:
            raise cv.Invalid(f"{radio_type} requires reset_pin.")
        if CONF_IRQ_PIN not in config:
            raise cv.Invalid(f"{radio_type} requires irq_pin.")
        if CONF_GDO0_PIN in config or CONF_GDO2_PIN in config:
            raise cv.Invalid("gdo0_pin/gdo2_pin are only valid for CC1101. Use irq_pin for SX1262/SX1276.")
        if CONF_CC1101_ALLOW_EXPERIMENTAL in config and config.get(CONF_CC1101_ALLOW_EXPERIMENTAL, False):
            raise cv.Invalid("cc1101_allow_experimental is only valid for radio_type: CC1101.")

    if radio_type == "LR1121":
        if not config.get(CONF_LR1121_ALLOW_EXPERIMENTAL, False):
            raise cv.Invalid(
                "LR1121 support has never been run against hardware - it was written from the datasheet, "
                "the board schematic and the vendor package. Set lr1121_allow_experimental: true if you "
                "intend to be the one who finds out. Expect to check tcxo_voltage first (see examples/LR1121/). / "
                "Obsluga LR1121 nie byla nigdy uruchomiona na sprzecie - powstala z dokumentacji i schematu. "
                "Ustaw lr1121_allow_experimental: true, jesli chcesz to sprawdzic jako pierwszy."
            )
        if CONF_BUSY_PIN not in config:
            raise cv.Invalid(
                "LR1121 requires busy_pin. Every command on this chip is framed by the BUSY handshake; "
                "without that line the driver cannot tell 'not ready' from 'answered'."
            )
    else:
        if config.get(CONF_LR1121_ALLOW_EXPERIMENTAL, False):
            raise cv.Invalid("lr1121_allow_experimental is only valid for radio_type: LR1121.")
        for key in (CONF_TCXO_VOLTAGE, CONF_TCXO_STARTUP_TICKS, CONF_RX_BANDWIDTH, CONF_PREAMBLE_DETECTOR,
                    CONF_PAYLOAD_LENGTH, CONF_RX_BOOSTED, CONF_BITRATE, CONF_DEVIATION):
            # Defaults are always present, so only an explicit non-default value
            # is worth rejecting. Silently ignoring it would be worse: the user
            # would think they had tuned something.
            if key in config and config[key] != BASE_CONFIG_DEFAULTS_LR1121[key]:
                raise cv.Invalid(f"{key} is only valid for radio_type: LR1121.")

    return config


CONFIG_SCHEMA = cv.All(BASE_CONFIG_SCHEMA, _validate_radio_pins)


def _validate_framework(config):
    # Every build, example and CI target uses esp-idf; arduino is never exercised.
    # The difference is not cosmetic: arduino headers type uint32_t as
    # 'unsigned long' where esp-idf uses 'unsigned int', so format strings that
    # are clean here warn there. Configs written for other wMBus components
    # routinely carry 'framework: arduino' over, so refuse to produce an
    # untested binary unless the user opts in on purpose. Same shape as
    # cc1101_allow_experimental: explicit consent for an unverified path.
    # On esp-idf the flag is ignored - esp-idf always passes.
    if CORE.using_arduino and not config.get(CONF_ALLOW_UNTESTED_FRAMEWORK, False):
        raise cv.Invalid(
            "framework arduino is untested for this component - use esp-idf (see examples/), "
            "or set allow_untested_framework: true to build it anyway / "
            "framework arduino jest nieprzetestowany dla tego komponentu - uzyj esp-idf "
            "(patrz examples/), albo ustaw allow_untested_framework: true aby zbudowac mimo to."
        )
    return config


# Final validation: the target framework is only reliably known once every
# component has been validated, so this cannot live in CONFIG_SCHEMA.
FINAL_VALIDATE_SCHEMA = _validate_framework


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

        # External RF switch gate (Wio-SX1262 pin 1 / RF_SW)
        if CONF_RF_SW_PIN in config:
            p = await cg.gpio_pin_expression(config[CONF_RF_SW_PIN])
            cg.add(radio_var.set_rf_sw_pin(p))


    if config[CONF_RADIO_TYPE] == "LR1121":
        # The driver compiles only when a config asks for it. Without this flag
        # transceiver_lr1121.cpp is an empty translation unit, so a board that
        # works today cannot be broken by a driver nobody has run yet.
        cg.add_build_flag("-DUSE_WMBUS_RADIO_LR1121")

        LR1121TcxoVoltage = radio_ns.enum("LR1121TcxoVoltage")
        LR1121RxBandwidth = radio_ns.enum("LR1121RxBandwidth")
        LR1121PreambleDetector = radio_ns.enum("LR1121PreambleDetector")

        cg.add(radio_var.set_tcxo_voltage(
            getattr(LR1121TcxoVoltage, LR1121_TCXO_VOLTAGES[config[CONF_TCXO_VOLTAGE]])
        ))
        cg.add(radio_var.set_tcxo_startup_ticks(config[CONF_TCXO_STARTUP_TICKS]))
        cg.add(radio_var.set_rx_bandwidth(
            getattr(LR1121RxBandwidth, LR1121_RX_BANDWIDTHS[config[CONF_RX_BANDWIDTH]])
        ))
        cg.add(radio_var.set_preamble_detector(
            getattr(LR1121PreambleDetector, LR1121_PREAMBLE_DETECTORS[config[CONF_PREAMBLE_DETECTOR]])
        ))
        cg.add(radio_var.set_payload_length(config[CONF_PAYLOAD_LENGTH]))
        cg.add(radio_var.set_rx_boosted(config[CONF_RX_BOOSTED]))
        cg.add(radio_var.set_bitrate(config[CONF_BITRATE]))
        cg.add(radio_var.set_deviation(config[CONF_DEVIATION]))

    if config[CONF_RADIO_TYPE] == "SX1276" and CONF_TCXO_PIN in config:
        tcxo_pin = await cg.gpio_pin_expression(config[CONF_TCXO_PIN])
        cg.add(radio_var.set_tcxo_pin(tcxo_pin))

    if config[CONF_RADIO_TYPE] != "CC1101":
        reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
        cg.add(radio_var.set_reset_pin(reset_pin))

    ListenMode = radio_ns.enum("ListenMode", is_class=False)
    listen_mode_map = {
        "t1": ListenMode.LISTEN_MODE_T1,
        "c1": ListenMode.LISTEN_MODE_C1,
        "both": ListenMode.LISTEN_MODE_BOTH,
        "s1": ListenMode.LISTEN_MODE_S1,
    }
    effective_listen_mode = config[CONF_LISTEN_MODE]

    # Mode-aware frequency defaults:
    # - T1/C1/both keep the legacy 868.950 MHz default.
    # - S1 defaults to 868.300 MHz.
    # An explicit `frequency:` value in YAML always overrides the mode default.
    default_frequency_mhz = 868.300 if effective_listen_mode == "s1" else 868.950
    frequency_mhz = config[CONF_FREQUENCY] if CONF_FREQUENCY in config else default_frequency_mhz

    if config[CONF_RADIO_TYPE] == "CC1101":
        gdo0_pin = await cg.gpio_pin_expression(config[CONF_GDO0_PIN])
        gdo2_pin = await cg.gpio_pin_expression(config[CONF_GDO2_PIN])
        cg.add(radio_var.set_gdo0_pin(gdo0_pin))
        cg.add(radio_var.set_gdo2_pin(gdo2_pin))
        cg.add(radio_var.set_frequency_mhz(frequency_mhz))
        # Receiver task wake-up interrupt is the sync-detect line.
        cg.add(radio_var.set_irq_pin(gdo2_pin))
    else:
        cg.add(radio_var.set_frequency_mhz(frequency_mhz))

    cg.add(radio_var.set_listen_mode(listen_mode_map[effective_listen_mode]))

    if config[CONF_RADIO_TYPE] != "CC1101":
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
    cg.add(var.set_receiver_task_stack_size(config[CONF_RECEIVER_TASK_STACK_SIZE]))
    cg.add(var.set_listen_mode_filter_after_parse(config[CONF_LISTEN_MODE_FILTER_AFTER_PARSE]))

    topic_name = config.get(CONF_TOPIC_NAME) or CORE.name
    if not topic_name:
        topic_name = "wmbus"

    warnings = []

    # All builds, examples and the 2026.7.0 day-one verification use esp-idf.
    # Arduino may compile, but nobody tests it — say so at compile time and in
    # the boot log instead of letting tutorial-style configs drift onto an
    # unverified path silently.
    if CORE.using_arduino:
        arduino_warning = (
            "framework arduino is untested for this component - all builds and tests use esp-idf, "
            "see examples/ / framework arduino jest nieprzetestowany dla tego komponentu - "
            "wszystkie buildy i testy uzywaja esp-idf, patrz examples/."
        )
        _LOGGER.warning("[wmbus_radio] %s", arduino_warning)
        warnings.append(arduino_warning)

    if CONF_TELEGRAM_TOPIC in config and str(config.get(CONF_TELEGRAM_TOPIC, "")).strip():
        telegram_topic = config[CONF_TELEGRAM_TOPIC]
        warnings.append("telegram_topic is a legacy/manual override / telegram_topic to reczne ustawienie legacy. Prefer topic_name / zalecane topic_name.")
    else:
        telegram_topic = f"wmbus/{topic_name}/telegram"

    raw_diag_mode = config.get(CONF_DIAGNOSTIC_MODE, "off")
    diag_mode = _normalize_diagnostic_mode(raw_diag_mode)
    if raw_diag_mode != diag_mode:
        warnings.append(f"diagnostic_mode: {raw_diag_mode} is deprecated / jest przestarzale. Use / uzyj diagnostic_mode: {diag_mode}.")

    preset_map = {
        "off": {"verbose": False, "raw": False, "summary": False, "drop": False, "rx_path": False, "highlight_only": False, "suggestion": False, "summary_15min": False, "summary_60min": False, "meter_stats": "off"},
        "low": {"verbose": False, "raw": False, "summary": True, "drop": False, "rx_path": False, "highlight_only": False, "suggestion": False, "summary_15min": False, "summary_60min": False, "meter_stats": "off"},
        "normal": {"verbose": False, "raw": False, "summary": True, "drop": False, "rx_path": False, "highlight_only": False, "suggestion": True, "summary_15min": True, "summary_60min": False, "meter_stats": "highlighted"},
        "debug": {"verbose": False, "raw": False, "summary": True, "drop": True, "rx_path": True, "highlight_only": True, "suggestion": True, "summary_15min": True, "summary_60min": False, "meter_stats": "highlighted"},
        "dev": {"verbose": True, "raw": True, "summary": True, "drop": True, "rx_path": True, "highlight_only": False, "suggestion": True, "summary_15min": True, "summary_60min": True, "meter_stats": "all"},
    }
    diag_preset = preset_map[diag_mode]
    cg.add(var.set_diagnostic_mode_str(diag_mode))

    legacy_diag_options = [
        CONF_DIAG_VERBOSE,
        CONF_DIAG_PUBLISH_RAW,
        CONF_DIAG_PUBLISH_SUMMARY,
        CONF_DIAG_PUBLISH_DROP_EVENTS,
        CONF_DIAG_PUBLISH_RX_PATH_EVENTS,
        CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY,
        CONF_DIAG_PUBLISH_SUGGESTION,
        CONF_DIAG_PUBLISH_SUMMARY_15MIN,
        CONF_DIAG_PUBLISH_SUMMARY_60MIN,
        CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS,
    ]
    for opt in legacy_diag_options:
        if opt in config:
            warnings.append(f"{opt} is deprecated/advanced / {opt} jest przestarzale/zaawansowane. Prefer diagnostic_mode presets / zalecane presety diagnostic_mode.")

    if CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY in config and CONF_DIAG_EVENTS_HIGHLIGHT_ONLY not in config:
        warnings.append("diagnostic_publish_highlight_only is deprecated / jest przestarzale. Use diagnostic_events_highlight_only / uzyj diagnostic_events_highlight_only.")

    explicit_diag_enabled = any([
        config.get(CONF_DIAG_PUBLISH_SUMMARY, False),
        config.get(CONF_DIAG_PUBLISH_DROP_EVENTS, False),
        config.get(CONF_DIAG_PUBLISH_RX_PATH_EVENTS, False),
        config.get(CONF_DIAG_PUBLISH_SUMMARY_15MIN, False),
        config.get(CONF_DIAG_PUBLISH_SUMMARY_60MIN, False),
        config.get(CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS, False),
        config.get(CONF_DIAG_PUBLISH_SUGGESTION, False),
        CONF_DIAG_METER_STATS in config and config.get(CONF_DIAG_METER_STATS) != "off",
    ])

    if CONF_DIAG_TOPIC in config and str(config.get(CONF_DIAG_TOPIC, "")).strip():
        diag_topic = config[CONF_DIAG_TOPIC]
        warnings.append("diagnostic_topic is a legacy/manual override / diagnostic_topic to reczne ustawienie legacy. Prefer topic_name / zalecane topic_name.")
    elif diag_mode != "off" or explicit_diag_enabled:
        diag_topic = f"wmbus/{topic_name}/diag"
    else:
        diag_topic = ""

    # Always-on radio health pulse + ESP-side meter flags. Derived from
    # topic_name regardless of diagnostic_mode, so the addon can present Layer 1
    # ("ESP alive") and the ESP-flagged meter badge even when diagnostics are off.
    health_topic = f"wmbus/{topic_name}/health"
    meters_topic = f"wmbus/{topic_name}/meters"
    rssi_topic = f"wmbus/{topic_name}/rssi"
    rx_topic = f"wmbus/{topic_name}/rx"

    cg.add(var.set_diag_topic(diag_topic))
    cg.add(var.set_health_topic(health_topic))
    cg.add(var.set_meters_topic(meters_topic))
    cg.add(var.set_rssi_topic(rssi_topic))
    cg.add(var.set_rx_topic(rx_topic))
    cg.add(var.set_telegram_topic(telegram_topic))
    cg.add(var.set_target_meter_id_str(config.get(CONF_TARGET_METER_ID, "")))
    cg.add(var.set_target_topic(config.get(CONF_TARGET_TOPIC, "")))
    cg.add(var.set_target_log(config.get(CONF_TARGET_LOG, True)))
    cg.add(var.set_publish_radio_raw(config.get(CONF_PUBLISH_RADIO_RAW, False)))
    cg.add(var.set_publish_rssi(config.get(CONF_PUBLISH_RSSI, False)))

    # forward_meters accepts an explicit list, or `true` meaning "reuse
    # highlight_meters" so the same IDs do not have to be written twice.
    forward_meters = config.get(CONF_FORWARD_METERS, [])
    forward_meters_inherited = False
    if forward_meters is True:
        forward_meters_csv = _meters_csv(config.get(CONF_HIGHLIGHT_METERS, []))
        forward_meters_inherited = True
        if not forward_meters_csv:
            # Filtering on an empty list would silence the whole RAW stream, so
            # fall back to forwarding everything and say so loudly.
            warnings.append(
                "forward_meters: true but highlight_meters is empty - no filtering applied, "
                "every frame is forwarded / forward_meters: true, ale highlight_meters jest puste - "
                "filtr nie dziala, przekazywane sa wszystkie ramki."
            )
            forward_meters_inherited = False
    elif forward_meters is False:
        forward_meters_csv = ""
    else:
        forward_meters_csv = _meters_csv(forward_meters)
    cg.add(var.set_forward_meters_csv(forward_meters_csv))
    cg.add(var.set_forward_meters_inherited(forward_meters_inherited))

    diag_events_highlight_only = (
        config[CONF_DIAG_EVENTS_HIGHLIGHT_ONLY]
        if CONF_DIAG_EVENTS_HIGHLIGHT_ONLY in config
        else (config[CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY]
              if CONF_DIAG_PUBLISH_HIGHLIGHT_ONLY in config
              else diag_preset["highlight_only"])
    )
    meter_stats = config.get(CONF_DIAG_METER_STATS, diag_preset["meter_stats"])
    cg.add(var.set_diagnostic_meter_stats_str(meter_stats))
    summary_highlight = meter_stats in ("highlighted", "all")
    meter_stats_all = meter_stats == "all"
    if CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS in config:
        summary_highlight = config[CONF_DIAG_PUBLISH_SUMMARY_HIGHLIGHT_METERS]
        meter_stats_all = False

    cg.add(var.set_diag_verbose(config[CONF_DIAG_VERBOSE] if CONF_DIAG_VERBOSE in config else diag_preset["verbose"]))
    cg.add(var.set_diag_publish_raw(config[CONF_DIAG_PUBLISH_RAW] if CONF_DIAG_PUBLISH_RAW in config else diag_preset["raw"]))
    cg.add(var.set_diag_publish_summary(config[CONF_DIAG_PUBLISH_SUMMARY] if CONF_DIAG_PUBLISH_SUMMARY in config else diag_preset["summary"]))
    cg.add(var.set_diag_publish_drop_events(config[CONF_DIAG_PUBLISH_DROP_EVENTS] if CONF_DIAG_PUBLISH_DROP_EVENTS in config else diag_preset["drop"]))
    cg.add(var.set_diag_publish_rx_path_events(config[CONF_DIAG_PUBLISH_RX_PATH_EVENTS] if CONF_DIAG_PUBLISH_RX_PATH_EVENTS in config else diag_preset["rx_path"]))
    cg.add(var.set_diag_publish_highlight_only(diag_events_highlight_only))
    cg.add(var.set_diag_publish_suggestion(config[CONF_DIAG_PUBLISH_SUGGESTION] if CONF_DIAG_PUBLISH_SUGGESTION in config else diag_preset["suggestion"]))
    cg.add(var.set_diag_summary_interval_ms(config[CONF_DIAG_SUMMARY_INTERVAL].total_milliseconds))
    cg.add(var.set_diag_publish_summary_15min(config[CONF_DIAG_PUBLISH_SUMMARY_15MIN] if CONF_DIAG_PUBLISH_SUMMARY_15MIN in config else diag_preset["summary_15min"]))
    cg.add(var.set_diag_publish_summary_60min(config[CONF_DIAG_PUBLISH_SUMMARY_60MIN] if CONF_DIAG_PUBLISH_SUMMARY_60MIN in config else diag_preset["summary_60min"]))
    cg.add(var.set_diag_publish_summary_highlight_meters(summary_highlight))
    cg.add(var.set_diag_meter_stats_all(meter_stats_all))

    for warning in warnings:
        cg.add(var.add_config_warning(warning))

    SX1276BusyEtherMode = radio_ns.enum("SX1276BusyEtherMode", is_class=True)
    busy_ether_mode_map = {
        "normal": SX1276BusyEtherMode.NORMAL,
        "aggressive": SX1276BusyEtherMode.AGGRESSIVE,
        "adaptive": SX1276BusyEtherMode.ADAPTIVE,
    }
    cg.add(var.set_sx1276_busy_ether_mode(busy_ether_mode_map[config.get(CONF_SX1276_BUSY_ETHER_MODE, "normal")]))

    if config[CONF_RADIO_TYPE] == "SX1262":
        sx1262_dio2_rf = config.get(CONF_RF_SWITCH, config.get(CONF_DIO2_RF_SWITCH, True))
        cg.add(var.set_sx1262_yaml_sanity(
            config.get(CONF_HAS_TCXO, False),
            sx1262_dio2_rf,
            config.get(CONF_LONG_GFSK_PACKETS, False),
            config.get(CONF_RX_GAIN, "boosted"),
        ))

    if config[CONF_RADIO_TYPE] == "SX1276":
        cg.add(var.set_sx1276_yaml_sanity(CONF_TCXO_PIN in config))

    if config[CONF_RADIO_TYPE] == "SX1262":
        cg.add(var.set_sx1262_rf_sw_pin_configured(CONF_RF_SW_PIN in config))

    if config[CONF_RADIO_TYPE] == "CC1101":
        cg.add(var.set_cc1101_yaml_sanity(
            CONF_GDO0_PIN in config,
            CONF_GDO2_PIN in config,
        ))

    for _line in _config_report_lines(config):
        cg.add(var.add_config_report_line(_line))

    # Log highlight config
    meters = config.get(CONF_HIGHLIGHT_METERS, [])
    meters_csv = _meters_csv(meters)
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
