# Radio options

[Polska wersja](RADIO_OPTIONS_MINIMAL_PL.md)

## Common

| Radio | Required pins | Optional settings | Notes |
|---|---|---|---|
| `SX1262` | `cs_pin`, `reset_pin`, `irq_pin`, `busy_pin` | `frequency`, TCXO, FEM, RF-switch gate, `long_gfsk_packets` | recommended for difficult RF conditions and long frames |
| `SX1276` | `cs_pin`, `reset_pin`, `irq_pin` | `frequency`, `busy_pin`, `sx1276_busy_ether_mode`, `tcxo_pin` | suitable for quieter installations; has busy-ether handling; `tcxo_pin` only for boards with a separate TCXO-enable pin |
| `CC1101` | `cs_pin`, `gdo0_pin`, `gdo2_pin` | `frequency` | experimental; requires `cc1101_allow_experimental: true`; single IRQ is unsupported |
| `LR1121` | `cs_pin`, `reset_pin`, `irq_pin`, `busy_pin` | `frequency`, `tcxo_voltage`, `tcxo_startup_ticks`, `payload_length` | experimental; requires `lr1121_allow_experimental: true`; receives T1/C1/S1 on Waveshare HF hardware |

## Listen mode frequency defaults

| `listen_mode` | Default frequency | Notes |
|---|---:|---|
| `t1` | `868.950 MHz` | T1 only |
| `c1` | `868.950 MHz` | C1 only |
| `both` | `868.950 MHz` | T1/C1 only; does not include S1 |
| `s1` | `868.300 MHz` | experimental S1 only |

`frequency:` explicitly overrides the mode's default frequency on any radio. A typical S1 diagnostic example:

```yaml
wmbus_radio:
  radio_type: SX1262
  listen_mode: s1
  frequency: 868.36
```

S1 uses a different RF profile from T1/C1 and is therefore not included in `both`.

## SX1262

| Option | Default | Description |
|---|---:|---|
| `has_tcxo` | `false` | enable for TCXO modules, including some Heltec boards |
| `dio2_rf_switch` / `rf_switch` | `true` | RF-switch control through DIO2 |
| `rx_gain` | `boosted` | `boosted` or `power_saving` |
| `long_gfsk_packets` | `false` | long GFSK packet mode; **costs about 7 dB with weak signals** — enable only when receiving frames above ~150 decoded bytes |
| `min_preamble_bits` | `16` | preamble detector threshold; **16 is the maximum for T1**; `8` costs about 16% of meters heard (see `CONFIG_REFERENCE_MINIMAL.md`) |
| `clear_device_errors_on_boot` | `false` | clear latched device errors after startup |
| `publish_dev_err_after_clear` | `false` | publish the error-clear result |
| `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin` | none | FEM pins, e.g. Heltec V4 |
| `rf_sw_pin` | none | module RF-switch gate; required on XIAO ESP32-S3 + Wio-SX1262 (`GPIO38`) |

### `rf_sw_pin` and `dio2_rf_switch`

These serve different purposes; boards that require the gate need both.

`dio2_rf_switch` refers to the SX1262 chip's DIO2 output and selects the TX/RX
**direction**. `rf_sw_pin` is a host GPIO that opens the module RF-switch
**gate**, determining whether it conducts at all.

The Seeed Wio-SX1262 module exposes this gate on pin 1 (`RF_SW`); in the
XIAO ESP32-S3 kit it is connected to `GPIO38`:

```yaml
wmbus_radio:
  radio_type: SX1262
  rf_sw_pin: GPIO38
```

Without this option, sensitivity is about 30 dB lower. The symptom is not
silence: frames still decode and `DIAG hint` reports `GOOD`, but there are
several times fewer frames and RSSI values cluster just above the sensitivity threshold.

Do not control this pin with an `on_boot` action on a `gpio` output; see
[Troubleshooting](TROUBLESHOOTING.md), the section about receivers hearing few meters.

Heltec V3 / V4 / V4-R8 boards do not require this option; see their board
examples for any `fem_*` settings.

## SX1276

| Option | Default | Description |
|---|---:|---|
| `sx1276_busy_ether_mode` | `normal` | `normal`, `aggressive`, `adaptive` |
| `tcxo_pin` | none | optional TCXO-enable pin; SX1276 only |

`busy_ether_state` in the summary reports the busy-ether mechanism and is meaningful only on SX1276.

The YAML schema also accepts `sx1276_busy_ether_mode` for other radios, but
SX1262/CC1101/LR1121 ignore it without error and report `n/a` in the summary.

`tcxo_pin` is an explicit option for SX1276 boards with a separate TCXO-enable
pin. If set, the component drives it HIGH before radio initialization.
Ordinary SX1276 boards do not need this option.

Example for LILYGO T3 V3.0 TCXO OLED LoRa32:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

The component does not automatically detect the board model or TCXO wiring.
Check the schematic or manufacturer documentation.

## CC1101

Minimal configuration:

```yaml
wmbus_radio:
  - radio_type: CC1101
    cc1101_allow_experimental: true
    cs_pin: GPIO4
    gdo0_pin: GPIO3
    gdo2_pin: GPIO2
    frequency: 868.95
```

Do not copy CC1101 wiring from single-IRQ projects. This component requires
separate GDO0 and GDO2 connections.

## LR1121

Minimal configuration (Waveshare ESP32-S3-LR1121-HF, SKU 34011):

```yaml
wmbus_radio:
  - radio_type: LR1121
    lr1121_allow_experimental: true
    cs_pin: GPIO42
    reset_pin: GPIO39
    irq_pin: GPIO38
    busy_pin: GPIO41
    tcxo_voltage: 3.0v
    payload_length: 255
```

Three things that can make a functioning board appear dead:

- **`tcxo_voltage: 3.0v`** — measured. At `1.8v`, the chip reports
  `HF_XOSC_START` and never reaches reception. The vendor package gives both
  values for this board; use the one verified on hardware.
- **`payload_length: 255`** — NES frames contain 245 raw bytes. A lower value truncates them.
- **Antenna socket.** The board has multiple u.FL sockets: ESP32 WiFi, the
  LR1121 2.4 GHz port, and the sub-GHz path through the RF switch. The wrong
  socket produces complete silence without an error.

This board has no USB-UART bridge. The console uses the ESP32-S3 native USB,
so `logger:` needs `hardware_uart: USB_SERIAL_JTAG` or no log will appear.

The startup `HF_XOSC_START` message **is normal and is not a fault**: it
latches on entering `STDBY_XOSC`, and both subsequent calibrations complete
without errors. The driver explains this in the log.
