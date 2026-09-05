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
#
# Deliberately still empty. The optional sensor:/number:/select: platforms of
# this component are used by a minority of configs, and auto-loading their core
# components would make every user compile three extra base components just so
# mqtt_outbox.cpp could include their headers - the opposite of an opt-in
# feature. Those includes are now guarded by USE_SENSOR / USE_NUMBER /
# USE_SELECT, which ESPHome defines for whichever components a config actually
# loads; declaring `sensor: - platform: wmbus_radio` loads the sensor component
# on its own, so nothing is lost by keeping this empty.
AUTO_LOAD = []

MULTI_CONF = True

CONF_RADIO_ID = "radio_id"
CONF_ON_FRAME = "on_frame"
CONF_RADIO_TYPE = "radio_type"
CONF_MARK_AS_HANDLED = "mark_as_handled"
CONF_BUSY_PIN = "busy_pin"
CONF_LISTEN_MODE = "listen_mode"
CONF_LISTEN_MODE_FILTER_AFTER_PARSE = "listen_mode_filter_after_parse"
CONF_USE_NOISE_FLOOR_THRESHOLD = "use_noise_floor_threshold"
CONF_NOISE_FLOOR_MARGIN_DB = "noise_floor_margin_db"
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

# Per-topic MQTT QoS (0/1/2). Every default below matches the value that was
# previously hardcoded, so an existing YAML with none of these set keeps
# publishing exactly as before.
CONF_TELEGRAM_QOS = "telegram_qos"
CONF_RSSI_QOS = "rssi_qos"
CONF_HEALTH_QOS = "health_qos"
CONF_DIAGNOSTIC_QOS = "diagnostic_qos"
CONF_RX_QOS = "rx_qos"

# RAM store-and-forward buffer for the telegram/rx-metadata stream while MQTT
# is disconnected. 0 disables buffering (pre-existing behaviour: drop).
CONF_MQTT_BUFFER_SIZE = "mqtt_buffer_size"

# Per-meter share of the RAM buffer, only meaningful with a non-empty
# forward_meters whitelist. Plain positive integer weights (not percentages)
# so there is no total the user has to make add up correctly - see
# Radio::recompute_buffer_quotas_() in mqtt_outbox.cpp for how weights become
# quotas that always sum to exactly the current buffer capacity.
CONF_BUFFER_PRIORITY = "buffer_priority"

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

# SX1262 T1 receiver bandwidth. C1 and S1 are not affected - their 234.3 kHz
# is a measured optimum (three-point sweep on S-mode, 2026-08-01) and is
# pinned in the driver. T1 never got that sweep: its 312.0 kHz is inherited,
# and 25% wider than the window the SX1276 uses for the same mode.
# SX1262 preamble detector: how many preamble bits the radio must see before it
# starts a reception. Upstream hardcodes 8; the LR1121 driver uses 16. Measured
# 2026-09-01: this is the single knob that decides how aggressively a board
# attempts weak frames, so it belongs in YAML rather than in the source.
SX1262_PREAMBLE_DETECTORS = {
    0: "PREAMBLE_DETECT_OFF",
    8: "PREAMBLE_DETECT_8",
    16: "PREAMBLE_DETECT_16",
    24: "PREAMBLE_DETECT_24",
    32: "PREAMBLE_DETECT_32",
}

CONF_SX1262_RX_BANDWIDTH = "sx1262_rx_bandwidth"
SX1262_T1_RX_BANDWIDTHS = {
    "312khz": "T1_BW_312",
    "234khz": "T1_BW_234",
    "156khz": "T1_BW_156",
}

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
CONF_SPI_DATA_RATE = "spi_data_rate"
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
# Minimum preamble bits the receiver must see before it starts a reception.
# Chip-agnostic on purpose: SX1262 and LR1121 expose the same knob under
# different names. NOT the transmit preamble length - that is a separate
# SetPacketParams field and is not configurable here.
CONF_MIN_PREAMBLE_BITS = "min_preamble_bits"
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

# Same key (tcxo_voltage), same eight datasheet steps, same reason it exists:
# proven on LR1121 (Waveshare HF_XOSC_START tuning), now extended to SX1262
# because a board finally showed up (LilyGO T-Beam v1.2) that isn't 3.0V.
SX1262_TCXO_VOLTAGES = {
    "1.6v": "SX1262_TCXO_1_6V",
    "1.7v": "SX1262_TCXO_1_7V",
    "1.8v": "SX1262_TCXO_1_8V",
    "2.2v": "SX1262_TCXO_2_2V",
    "2.4v": "SX1262_TCXO_2_4V",
    "2.7v": "SX1262_TCXO_2_7V",
    "3.0v": "SX1262_TCXO_3_0V",
    "3.3v": "SX1262_TCXO_3_3V",
}

LR1121_RX_BANDWIDTHS = {
    "234300": "LR1121_BW_234300",
    "312000": "LR1121_BW_312000",
    "373600": "LR1121_BW_373600",
    "467000": "LR1121_BW_467000",
}

LR1121_PREAMBLE_DETECTORS = {
    0: "LR1121_PREAMBLE_OFF",
    8: "LR1121_PREAMBLE_MIN_8B",
    16: "LR1121_PREAMBLE_MIN_16B",
    24: "LR1121_PREAMBLE_MIN_24B",
    32: "LR1121_PREAMBLE_MIN_32B",
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
    # Shared by SX1262/SX1276/LR1121, not LR1121-only - see the schema entry.
    CONF_MIN_PREAMBLE_BITS: 16,
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


def _priority_csv(mapping):
    """Join a YAML buffer_priority {meter_id: weight} mapping into the
    "<id>:<weight>,<id>:<weight>,..." CSV string parsed by
    parse_meter_priority_csv_ in component.cpp. Keys are already validated
    meter-ID strings (_validate_meter_id), so no further quoting is needed."""
    return ",".join(f"{str(k).strip()}:{v}" for k, v in mapping.items())


def _validate_mqtt_buffer_size(value):
    """Either an explicit MESSAGE count, or "auto" to size the buffer from free
    heap/PSRAM at runtime (see Radio::suggested_mqtt_outbox_capacity_ in
    mqtt_outbox.cpp - re-evaluated periodically, not just once at boot).

    UNITS: this counts queued MQTT MESSAGES, not telegrams. One received
    telegram enqueues TWO of them - the raw frame on .../telegram and its
    metadata companion (rssi_dbm + received_at) on .../rx, which is published
    for every forwarded frame because rx_topic is always configured. So
    mqtt_buffer_size: 64 survives roughly 32 telegrams, not 64. Every figure
    in this feature uses the same unit (the buffer_depth / buffer_dropped_*
    sensors, the buffer_capacity number, the per-meter buffer_priority
    quotas), so there is exactly one unit to reason about - but it is
    messages, and halving it is the number of readings you actually keep.

    The explicit-number path keeps a sanity cap (8192): typos like an extra
    zero should fail validation, not silently compile. It is a soft ceiling
    either way - the C++ side refuses to grow the buffer once free heap (or
    free PSRAM, on a board whose payloads live there) drops below its safety
    reserve, regardless of what number is configured here. On a board without
    PSRAM a value in the thousands is simply unreachable and the runtime
    reserve pins the effective size far lower; "auto" is the sane choice
    there. On a PSRAM board a few thousand frames is realistic.
    """
    if isinstance(value, str) and value.strip().lower() == "auto":
        return "auto"
    return cv.int_range(min=0, max=8192)(value)


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
            # SPI clock for the radio device only, not for the whole bus.
            #
            # The driver defaults to 2 MHz, which is well inside every supported
            # part's rating. What it is not inside is every user's wiring: a
            # module on dupont leads has enough capacitance and crosstalk to drop
            # bits at 2 MHz on an otherwise healthy 3.3 V board. That failure is
            # silent - registers read back as their reset defaults and the radio
            # simply behaves as if it were misconfigured.
            #
            # Lower this before suspecting the part. Raising it above 2 MHz is
            # allowed but has no known benefit here: the RX FIFO drain is paced
            # by the radio, not by the bus.
            cv.Optional(CONF_SPI_DATA_RATE): cv.All(
                cv.frequency, cv.Range(min=100000, max=8000000)
            ),
            cv.Optional(CONF_ALLOW_UNTESTED_FRAMEWORK, default=False): cv.boolean,
            cv.Optional(CONF_FREQUENCY): cv.float_range(min=300.0, max=928.0),
            cv.Optional(CONF_LISTEN_MODE, default="both"): cv.one_of(
                "t1", "c1", "s1", "both", lower=True
            ),
            # Advanced/experimental. Default false keeps the legacy behavior:
            # filter listen_mode by preliminary raw packet mode before parsing.
            # True tries parser/CRC-selected mode first, then filters afterwards.
            cv.Optional(CONF_LISTEN_MODE_FILTER_AFTER_PARSE, default=False): cv.boolean,
            # Derive the weak-start abort threshold from the measured noise floor
            # instead of an average of successful receptions. Default OFF: the
            # measurement (noise_floor_dbm in the diagnostic summary) ships enabled
            # so the right margin can be chosen from real numbers on real
            # front-ends before anyone's radio behaviour changes.
            cv.Optional(CONF_USE_NOISE_FLOOR_THRESHOLD, default=False): cv.boolean,
            # How far above the floor a start must be to be worth attempting.
            # Small on purpose: this decides whether to TRY, and a failed attempt
            # costs a few milliseconds while a refused one costs the whole frame.
            cv.Optional(CONF_NOISE_FLOOR_MARGIN_DB, default=6): cv.int_range(min=0, max=30),
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

            # Shared by SX1262, SX1276 and LR1121 - deliberately NOT in the
            # LR1121 block below. All three gate the start of a reception on a
            # minimum preamble length, and the 2026-09-01 measurements made that
            # gate the single knob deciding how aggressively a board attempts
            # weak frames: 8 bits cost ~16% of the meters heard, reproduced on
            # two independent antenna-matched pairs. CC1101 has the same gate
            # (PQT) but in different units, and SX1276 has no 32-bit setting;
            # both are rejected in the validator with a reason.
            #
            # The default is read out of BASE_CONFIG_DEFAULTS_LR1121 because
            # that dict is the single source for schema defaults and the
            # validator compares against it to tell "the user set this" from
            # "the schema injected it". It stays there despite the name.
            cv.Optional(CONF_MIN_PREAMBLE_BITS, default=BASE_CONFIG_DEFAULTS_LR1121[CONF_MIN_PREAMBLE_BITS]): cv.one_of(
                0, 8, 16, 24, 32, int=True
            ),

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

            # Per-topic MQTT QoS. Recommended: QoS 1 on the telegram/rx path
            # once mqtt_buffer_size > 0 (the buffered replay then also lands
            # at-least-once on reconnect). Defaults keep the pre-existing
            # hardcoded values, so an untouched YAML publishes identically.
            cv.Optional(CONF_TELEGRAM_QOS, default=0): cv.int_range(min=0, max=2),
            cv.Optional(CONF_RSSI_QOS, default=0): cv.int_range(min=0, max=2),
            cv.Optional(CONF_HEALTH_QOS, default=0): cv.int_range(min=0, max=2),
            cv.Optional(CONF_DIAGNOSTIC_QOS, default=0): cv.int_range(min=0, max=2),
            cv.Optional(CONF_RX_QOS, default=1): cv.int_range(min=0, max=2),

            # RAM outbox ceiling for the telegram + /rx metadata stream while
            # MQTT is disconnected, counted in MESSAGES (two per telegram -
            # see _validate_mqtt_buffer_size). "auto" sizes it from free
            # heap/PSRAM at runtime. Runtime-adjustable downward via number:
            # buffer_capacity if declared; see the sensor:/number:/select:
            # platforms.
            #
            # DEFAULT 0 = OFF, i.e. opt-in. Store-and-forward is not a free
            # improvement that can be switched on for everybody: it changes
            # MQTT delivery semantics (a reconnect replays a burst of
            # telegrams whose received_at is minutes old, which a backend that
            # timestamps on arrival will render as a step) and it spends
            # internal heap on boards without PSRAM. An existing config that
            # is upgraded must keep behaving exactly as it did, so switching
            # this on is a decision the user makes in YAML.
            cv.Optional(CONF_MQTT_BUFFER_SIZE, default=0): _validate_mqtt_buffer_size,

            # Per-meter RAM buffer share, only meaningful with a non-empty
            # forward_meters whitelist. Every key must be a meter ID already
            # usable in forward_meters/highlight_meters; the value is a plain
            # weight (not a percentage - no total to make add up), e.g.:
            #   buffer_priority:
            #     "12345678": 3   # gets 3x the buffer share of a default (1) meter
            #     "0x417F0666": 1
            # A whitelisted meter with no entry here defaults to weight 1, so
            # leaving this unset entirely means an equal split.
            cv.Optional(CONF_BUFFER_PRIORITY, default={}): cv.Schema(
                {_validate_meter_id: cv.int_range(min=1, max=1000)}
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
            cv.Optional(CONF_SX1262_RX_BANDWIDTH, default="312khz"): cv.one_of(
                *SX1262_T1_RX_BANDWIDTHS, lower=True
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
    if key == CONF_BUFFER_PRIORITY:
        if not value:
            return "disabled (shared buffer)"
        return f"{len(value)} meter(s): " + ", ".join(f"{k}:w{v}" for k, v in value.items())
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


_REPORT_BUS = (CONF_SPI_DATA_RATE,)
_REPORT_PINS = ("cs_pin", CONF_RESET_PIN, CONF_IRQ_PIN, CONF_BUSY_PIN,
                CONF_GDO0_PIN, CONF_GDO2_PIN, CONF_TCXO_PIN, CONF_RF_SW_PIN,
                CONF_FEM_EN_PIN, CONF_FEM_CTRL_PIN, CONF_FEM_PA_PIN)

_REPORT_CORE = (CONF_RADIO_TYPE, CONF_LISTEN_MODE, CONF_LISTEN_MODE_FILTER_AFTER_PARSE,
                CONF_USE_NOISE_FLOOR_THRESHOLD, CONF_NOISE_FLOOR_MARGIN_DB,
                CONF_FREQUENCY, CONF_RECEIVER_TASK_STACK_SIZE, CONF_ALLOW_UNTESTED_FRAMEWORK)

_REPORT_RADIO = {
    "SX1262": (CONF_HAS_TCXO, CONF_TCXO_VOLTAGE, CONF_DIO2_RF_SWITCH, CONF_RF_SWITCH, CONF_RX_GAIN,
               CONF_LONG_GFSK_PACKETS, CONF_CLEAR_DEVICE_ERRORS_ON_BOOT,
               CONF_PUBLISH_DEV_ERR_AFTER_CLEAR, CONF_SX1262_RX_BANDWIDTH,
               CONF_MIN_PREAMBLE_BITS),
    "SX1276": (CONF_SX1276_BUSY_ETHER_MODE, CONF_MIN_PREAMBLE_BITS),
    "CC1101": (CONF_CC1101_ALLOW_EXPERIMENTAL,),
    "LR1121": (CONF_LR1121_ALLOW_EXPERIMENTAL, CONF_TCXO_VOLTAGE, CONF_TCXO_STARTUP_TICKS,
               CONF_RX_BANDWIDTH, CONF_MIN_PREAMBLE_BITS, CONF_PAYLOAD_LENGTH,
               CONF_RX_BOOSTED, CONF_BITRATE, CONF_DEVIATION),
}

_REPORT_OUTPUT = (CONF_TOPIC_NAME, CONF_TELEGRAM_TOPIC, CONF_PUBLISH_RSSI,
                  CONF_FORWARD_METERS, CONF_TARGET_METER_ID, CONF_PUBLISH_RADIO_RAW,
                  CONF_TELEGRAM_QOS, CONF_RSSI_QOS, CONF_HEALTH_QOS, CONF_DIAGNOSTIC_QOS,
                  CONF_RX_QOS, CONF_MQTT_BUFFER_SIZE, CONF_BUFFER_PRIORITY)

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
        ("bus", _REPORT_BUS),
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
        for key in (CONF_TCXO_STARTUP_TICKS, CONF_RX_BANDWIDTH,
                    CONF_PAYLOAD_LENGTH, CONF_RX_BOOSTED, CONF_BITRATE, CONF_DEVIATION):
            # Defaults are always present, so only an explicit non-default value
            # is worth rejecting. Silently ignoring it would be worse: the user
            # would think they had tuned something.
            if key in config and config[key] != BASE_CONFIG_DEFAULTS_LR1121[key]:
                raise cv.Invalid(f"{key} is only valid for radio_type: LR1121.")

        # tcxo_voltage is shared with SX1262 (2026-08-26: SX1262's DIO3-TCXO
        # voltage is now configurable too, same key, same eight datasheet
        # steps) - so it is exempt from the LR1121-only loop above, but still
        # rejected for radios that have no TCXO-voltage command at all
        # (SX1276 uses tcxo_pin instead; CC1101 has no TCXO concept).
        if (radio_type != "SX1262" and CONF_TCXO_VOLTAGE in config
                and config[CONF_TCXO_VOLTAGE] != BASE_CONFIG_DEFAULTS_LR1121[CONF_TCXO_VOLTAGE]):
            raise cv.Invalid("tcxo_voltage is only valid for radio_type: LR1121 or SX1262.")

        # min_preamble_bits is shared with SX1262 and SX1276 (2026-09-01): all
        # three gate the start of a reception on a minimum preamble length, and
        # that gate turned out to be the single knob deciding how aggressively a
        # board attempts weak frames. Same key, same values, three drivers - so
        # it is exempt from the LR1121-only loop above.
        #
        # Rejected elsewhere because no other driver here implements it, NOT
        # because the hardware lacks it. The CC1101 has the same gate as PQT
        # (PKTCTRL1 bits 7:5) - this driver pins PKTCTRL1 to 0x00, i.e. PQT=0,
        # the fully permissive end, and verifies it stays there. Its units are
        # quality steps 0-7, not preamble bits, so it cannot share this key's
        # value space even if it were wired up. SX1276 gates reception
        # differently again (its own abort machinery).
        if CONF_MIN_PREAMBLE_BITS in config and config[CONF_MIN_PREAMBLE_BITS] != BASE_CONFIG_DEFAULTS_LR1121[CONF_MIN_PREAMBLE_BITS]:
            if radio_type == "CC1101":
                raise cv.Invalid(
                    "min_preamble_bits is not supported on CC1101. The chip has the same gate "
                    "(PQT, PKTCTRL1 bits 7:5) but this driver pins PKTCTRL1 to 0x00 and verifies "
                    "it stays there, and PQT counts quality steps 0-7, not preamble bits. / "
                    "min_preamble_bits nie jest obsługiwane na CC1101. Układ ma tę samą bramkę "
                    "(PQT, bity 7:5 rejestru PKTCTRL1), ale ten sterownik ustawia PKTCTRL1 na 0x00 "
                    "i tego pilnuje, a PQT liczy stopnie jakości 0-7, nie bity preambuły."
                )
            if radio_type == "SX1276" and config[CONF_MIN_PREAMBLE_BITS] == 32:
                raise cv.Invalid(
                    "min_preamble_bits: 32 is not available on SX1276 - RegPreambleDetect sizes "
                    "the detector in 1/2/3 bytes, so 8, 16 and 24 are the only non-zero values. / "
                    "min_preamble_bits: 32 nie istnieje na SX1276 - RegPreambleDetect wymiarowuje "
                    "detektor w 1/2/3 bajtach, więc 8, 16 i 24 to jedyne wartości niezerowe."
                )

    # min_preamble_bits above 16 makes a T1 receiver deaf. Measured on hardware
    # 2026-09-03: an SX1262 on listen_mode: t1 with min_preamble_bits: 24 took
    # 184 receiver triggers over two minutes and decoded ZERO frames. The
    # detector waits for 24 bits of continuous preamble, the T1 preamble is
    # shorter than that, so detection never completes and every frame flies
    # past. 16 works, so the usable preamble is 16..23 bits - consistent with
    # the 19 commonly quoted for T-mode.
    #
    # So 24 and 32 are not stricter thresholds for T1, they are values outside
    # its range, and a board configured that way receives nothing at all while
    # looking healthy: the radio triggers, the frame count stays at zero, and
    # nothing in the logs points at the preamble unless you know to look. That
    # is exactly the failure worth refusing at validation time rather than
    # discovering on a roof. Applies to every radio - this is a property of the
    # T1 signal, not of the chip - and to `both`, which includes T1.
    #
    # C1 and S1 are left alone: their preambles were not measured here, and
    # guessing in the direction that silently disables a receiver is the wrong
    # way to be wrong.
    if (config.get(CONF_MIN_PREAMBLE_BITS, 0) > 16
            and config.get(CONF_LISTEN_MODE) in ("t1", "both")):
        raise cv.Invalid(
            "min_preamble_bits: {} makes a T1 receiver deaf - the T1 preamble is shorter than "
            "24 bits, so the detector never completes and no frame is ever decoded (measured: "
            "184 triggers, 0 frames). Use 16 (the default) or 8 for listen_mode {}. / "
            "min_preamble_bits: {} czyni odbiornik T1 gluchym - preambula T1 jest krotsza niz "
            "24 bity, wiec detektor nigdy nie konczy detekcji i zadna ramka nie zostaje "
            "zdekodowana (zmierzone: 184 wyzwolenia, 0 ramek). Uzyj 16 (domyslnie) albo 8 dla "
            "listen_mode {}.".format(
                config[CONF_MIN_PREAMBLE_BITS], config.get(CONF_LISTEN_MODE),
                config[CONF_MIN_PREAMBLE_BITS], config.get(CONF_LISTEN_MODE))
        )

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

    # Must run before spi_setup(), which reads data_rate_ when it registers the
    # device with the bus. Left alone, the template default (2 MHz) stands.
    if CONF_SPI_DATA_RATE in config:
        cg.add(radio_var.set_data_rate(int(config[CONF_SPI_DATA_RATE])))

    if config[CONF_RADIO_TYPE] == "SX1262":
        dio2_rf = config.get(CONF_RF_SWITCH, config.get(CONF_DIO2_RF_SWITCH, True))
        cg.add(radio_var.set_dio2_rf_switch(dio2_rf))
        cg.add(radio_var.set_has_tcxo(config.get(CONF_HAS_TCXO, False)))

        SX1262TcxoVoltage = radio_ns.enum("SX1262TcxoVoltage")
        cg.add(radio_var.set_tcxo_voltage(
            getattr(SX1262TcxoVoltage, SX1262_TCXO_VOLTAGES[config[CONF_TCXO_VOLTAGE]])
        ))

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

        SX1262T1RxBandwidth = radio_ns.enum("SX1262T1RxBandwidth")
        cg.add(
            radio_var.set_t1_rx_bandwidth(
                getattr(
                    SX1262T1RxBandwidth,
                    SX1262_T1_RX_BANDWIDTHS[config.get(CONF_SX1262_RX_BANDWIDTH, "312khz")],
                )
            )
        )
        SX1262PreambleDetector = radio_ns.enum("SX1262PreambleDetector")
        cg.add(
            radio_var.set_preamble_detector(
                getattr(
                    SX1262PreambleDetector,
                    SX1262_PREAMBLE_DETECTORS[config.get(CONF_MIN_PREAMBLE_BITS, 16)],
                )
            )
        )

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
            getattr(LR1121PreambleDetector, LR1121_PREAMBLE_DETECTORS[config[CONF_MIN_PREAMBLE_BITS]])
        ))
        cg.add(radio_var.set_payload_length(config[CONF_PAYLOAD_LENGTH]))
        cg.add(radio_var.set_rx_boosted(config[CONF_RX_BOOSTED]))
        cg.add(radio_var.set_bitrate(config[CONF_BITRATE]))
        cg.add(radio_var.set_deviation(config[CONF_DEVIATION]))

    if config[CONF_RADIO_TYPE] == "SX1276":
        cg.add(radio_var.set_min_preamble_bits(config[CONF_MIN_PREAMBLE_BITS]))

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
    cg.add(var.set_use_noise_floor_threshold(config[CONF_USE_NOISE_FLOOR_THRESHOLD]))
    cg.add(var.set_noise_floor_margin_db(config[CONF_NOISE_FLOOR_MARGIN_DB]))

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

    cg.add(var.set_telegram_qos(config[CONF_TELEGRAM_QOS]))
    cg.add(var.set_rssi_qos(config[CONF_RSSI_QOS]))
    cg.add(var.set_health_qos(config[CONF_HEALTH_QOS]))
    cg.add(var.set_diag_qos(config[CONF_DIAGNOSTIC_QOS]))
    cg.add(var.set_rx_qos(config[CONF_RX_QOS]))

    _buffer_size = config[CONF_MQTT_BUFFER_SIZE]
    if _buffer_size == "auto":
        cg.add(var.set_mqtt_outbox_auto(True))
        # Real ceiling is computed on-device from free heap/PSRAM at setup()
        # and re-checked periodically (see mqtt_outbox.cpp); this is just a
        # safe, small starting point in case that first computation is skipped.
        cg.add(var.set_mqtt_outbox_max_capacity(16))
        cg.add(var.set_mqtt_outbox_capacity(16))
    else:
        cg.add(var.set_mqtt_outbox_max_capacity(_buffer_size))
        cg.add(var.set_mqtt_outbox_capacity(_buffer_size))

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

    buffer_priority = config.get(CONF_BUFFER_PRIORITY, {})
    if buffer_priority and not forward_meters_csv:
        # forward_meters_csv is the RESOLVED whitelist (after the true/false/
        # inherited handling above), so this also catches "forward_meters: true"
        # resolving to nothing because highlight_meters was empty too.
        warnings.append(
            "buffer_priority is set but forward_meters resolves to an empty whitelist - "
            "there is no per-meter whitelist to prioritise, ignoring / "
            "ustawiono buffer_priority, ale forward_meters jest puste - brak whitelisty "
            "do priorytetyzacji, ignorowanie."
        )
    cg.add(var.set_buffer_priority_csv(_priority_csv(buffer_priority)))

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
