# SPDX-License-Identifier: GPL-3.0-or-later
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import spi
from esphome.const import CONF_ID, CONF_FREQUENCY, CONF_MODE, CONF_RESET_PIN

CODEOWNERS = ["@Kustonium"]

# MQTT diagnostics are optional and compiled only when MQTT is configured.
DEPENDENCIES = ["esp32", "spi"]
AUTO_LOAD = []
MULTI_CONF = True

CONF_DCLK_PIN = "dclk_pin"
CONF_DATA_GPIO = "data_gpio"
CONF_INTERVAL = "interval"
CONF_FRAME = "frame"
CONF_POWER = "power"

wmbus_tx_ns = cg.esphome_ns.namespace("wmbus_tx")
SX1276Transmitter = wmbus_tx_ns.class_("SX1276Transmitter", cg.Component, spi.SPIDevice)
TxMode = wmbus_tx_ns.enum("TxMode", is_class=False)

MODES = {
    "t1": TxMode.TX_MODE_T1,
    "c1": TxMode.TX_MODE_C1,
    "s1": TxMode.TX_MODE_S1,
}

# Mode-appropriate defaults, same as the receiving side uses.
_DEFAULT_FREQUENCY_MHZ = {"t1": 868.950, "c1": 868.950, "s1": 868.300}


def _validate_frame(value):
    """The complete link-layer body in hex, starting with the L-field.

    Validating the L-field here is the point: a frame whose length byte
    disagrees with its contents still transmits, and the receiver then rejects
    or truncates it for reasons that look like an RF fault rather than a typo.
    """
    text = str(value).strip()
    for sep in (" ", "-", ":", "\t", "\n"):
        text = text.replace(sep, "")
    if text[:2].lower() == "0x":
        text = text[2:]

    if not text:
        raise cv.Invalid("frame cannot be empty / ramka nie moze byc pusta")
    if len(text) % 2:
        raise cv.Invalid(
            f"frame needs an even number of hex digits, got {len(text)} / "
            f"ramka wymaga parzystej liczby cyfr szesnastkowych"
        )
    try:
        data = bytes.fromhex(text)
    except ValueError:
        raise cv.Invalid(f"frame is not valid hex: '{text}' / to nie jest poprawny hex")

    if len(data) < 11:
        raise cv.Invalid(
            f"frame must be at least 11 bytes (L-field plus a 10-byte first block), got {len(data)} / "
            f"ramka musi miec co najmniej 11 bajtow"
        )
    if len(data) > 256:
        raise cv.Invalid(f"frame is limited to 256 bytes, got {len(data)} / limit to 256 bajtow")
    if data[0] + 1 != len(data):
        raise cv.Invalid(
            f"frame L-field is 0x{data[0]:02X} ({data[0]}), so the frame should be {data[0] + 1} "
            f"bytes, but it is {len(data)}. The first byte is the L-field and counts everything "
            f"after it / pierwszy bajt to L-field i liczy wszystko po nim"
        )
    return list(data)


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SX1276Transmitter),
            cv.Required(CONF_RESET_PIN): pins.internal_gpio_output_pin_schema,
            # SX1276 DIO1 supplies the bit clock in FSK continuous mode. Without
            # it there is nothing to clock the data stream against, so unlike on
            # the receiving side this pin is mandatory.
            cv.Required(CONF_DCLK_PIN): pins.internal_gpio_input_pin_schema,
            # Plain GPIO number: driven with the ESP-IDF gpio_set_level() API
            # inside the bit loop, where an InternalGPIOPin round trip would be
            # too slow at 100 kbps.
            cv.Optional(CONF_DATA_GPIO, default=34): cv.int_range(min=0, max=48),
            cv.Required(CONF_FRAME): _validate_frame,
            cv.Optional(CONF_MODE, default="t1"): cv.one_of(*MODES, lower=True),
            cv.Optional(CONF_FREQUENCY): cv.float_range(min=300.0, max=928.0),
            cv.Optional(CONF_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
            # Output power in dBm on PA_BOOST. The datasheet range for that pin
            # is 2..17 dBm and the register takes power - 2 as its nibble.
            # 12 dBm is the value this component used before the option existed,
            # so leaving it out changes nothing.
            #
            # Stepping this down is how a receiver gets measured: the same frame,
            # the same distance, and the level at which each board stops decoding.
            cv.Optional(CONF_POWER, default=12): cv.int_range(min=2, max=17),
            cv.Optional("dclk_diagnostics", default=False): cv.boolean,
        }
    )
    .extend(spi.spi_device_schema())
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)

    reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(var.set_reset_pin(reset_pin))
    dclk_pin = await cg.gpio_pin_expression(config[CONF_DCLK_PIN])
    cg.add(var.set_dclk_pin(dclk_pin))

    mode = config[CONF_MODE]
    cg.add(var.set_mode(MODES[mode]))
    cg.add(var.set_data_gpio(config[CONF_DATA_GPIO]))
    cg.add(var.set_interval_ms(config[CONF_INTERVAL].total_milliseconds))
    cg.add(var.set_frame(config[CONF_FRAME]))

    frequency_mhz = config.get(CONF_FREQUENCY, _DEFAULT_FREQUENCY_MHZ[mode])
    cg.add(var.set_frequency_hz(int(round(frequency_mhz * 1000000))))
    cg.add(var.set_power_dbm(config[CONF_POWER]))
    cg.add(var.set_dclk_diagnostics(config["dclk_diagnostics"]))
