# ESPHome wM-Bus Bridge RAW-only

[Polska wersja](README_PL.md)

[![Release][release-shield]][releases]
[![License][license-shield]][license]
![Supports SX1262][sx1262-shield]
![Supports SX1276][sx1276-shield]
![Supports CC1101][cc1101-shield]
![Supports LR1121][lr1121-shield]

[![CI][ci-shield]][ci]
[![Commit activity][commits-shield]][commits]

<!-- Dynamic badges point to the stable repository users install from.
     Preserve these URLs when promoting documentation from dev. Avoid badges
     with a hard-coded maintenance year or ESPHome version; scheduled CI checks
     compatibility. -->

[release-shield]: https://img.shields.io/github/v/release/Kustonium/esphome-wmbus-bridge-rawonly
[releases]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/releases
[license-shield]: https://img.shields.io/github/license/Kustonium/esphome-wmbus-bridge-rawonly
[license]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/blob/main/LICENSE
[sx1262-shield]: https://img.shields.io/badge/SX1262-yes-green.svg
[sx1276-shield]: https://img.shields.io/badge/SX1276-yes-green.svg
[cc1101-shield]: https://img.shields.io/badge/CC1101-experimental-yellow.svg
[lr1121-shield]: https://img.shields.io/badge/LR1121-experimental-yellow.svg
[ci-shield]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/actions/workflows/ci.yml/badge.svg
[ci]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/actions/workflows/ci.yml
[commits-shield]: https://img.shields.io/github/commit-activity/y/Kustonium/esphome-wmbus-bridge-rawonly
[commits]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/commits/main

<a href="https://buymeacoffee.com/Kustonium"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" height="41"></a>

```text
meter -> SX1262 / SX1276 / CC1101 / LR1121 -> ESPHome wmbus_radio -> MQTT RAW HEX -> backend / wmbusmeters / Home Assistant
```

RAW-to-MQTT wireless M-Bus / wM-Bus radio bridge for ESPHome, focused on SX1262 and SX1276, with experimental CC1101 and LR1121 support.

> ✅ **Verified on ESPHome 2026.7.0** — clean builds on all three test boards in CI.

## What this project is

The ESP device is a radio receiver and MQTT publisher. It does not decode meter values on the ESP.

> 🌉 Together with the Home Assistant add-on [`homeassistant-wmbus-mqtt-bridge`](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge) this ESP is the **radio front-end of a distributed wM-Bus → Home Assistant gateway**: the ESP receives and forwards RAW HEX, the add-on decrypts and decodes. Unlike a monolithic wM-Bus gateway (radio + decoder in one box), the decode side is offloaded to Home Assistant — no meter drivers and no AES keys live on the ESP. The ESP also works standalone with any MQTT backend (Node-RED, a custom script), and the add-on accepts hex from any source (rtl-wmbus, another gateway, the replay tool) — the two cooperate, but neither depends on the other.

> 🧱 **Responsibility boundary.** This firmware is an MQTT client: it publishes to a topic and its scope ends there. The broker — authentication, ACLs, TLS, network exposure and any broker-to-broker bridging for remote/distributed setups (site A → internet → site B) — is the operator's responsibility. Keep the broker on your LAN and use a tunnel/VPN or TLS broker bridging for remote access; do not expose port 1883 to the internet.

It does:

- receive T1/C1 frames and experimental S1 frames,
- validate and normalize telegrams,
- publish valid RAW HEX telegrams to MQTT,
- publish RF diagnostics.

It does not:

- choose `wmbusmeters` drivers,
- decrypt AES payloads,
- create final meter-value sensors,
- replace `wmbusmeters`.

## Quick start

Use one of the YAML examples from `examples/`, then read the startup log before changing anything else.

> **Requirements.** This component builds and runs on the ESP-IDF framework (every example uses it) and publishes to absolute MQTT topics of the form `wmbus/<topic_name>/telegram` — there is no configurable topic prefix.

Recommended path:

1. Choose the matching board example from `examples/`.
2. Use `topic_name` or omit it and let the component use `esphome.name`.
3. Start with `listen_mode: t1` unless you know you need C1 or S1.
4. Start with `diagnostic_mode: normal`.
5. Check the boot sanity report and local `Have data` logs before debugging MQTT/backend.

## MQTT topic model

Recommended topic scheme:

```text
wmbus/<device>/telegram
wmbus/<device>/rssi/<meter_id>      # only with publish_rssi: true (default: false)
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
wmbus/<device>/diag/boot
```

The backend bridge should subscribe to:

```text
wmbus/+/telegram
```

Do not manually build topic paths in normal YAML. Use `topic_name`, or omit it and let the component use `esphome.name`.

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

If MQTT is unavailable, radio reception continues and frames remain visible in local logs. MQTT publishing is skipped with a throttled warning.

TLS, remote brokers and certificates belong to ESPHome's standard `mqtt:` configuration, not to `wmbus_radio`.

## Radio notes

### SX1262

SX1262 board-level options are explicit. The component does not guess board wiring.

Common options:

```yaml
wmbus_radio:
  radio_type: SX1262
  has_tcxo: true
  dio2_rf_switch: true
  rx_gain: boosted
  long_gfsk_packets: false
```

At boot, the component prints a multiline SX1262 YAML sanity report. Missing `has_tcxo: true` on TCXO-based boards can still allow the radio to initialize, but RX may be completely silent. The default `long_gfsk_packets: false` preserves sensitivity; enable it only for frames above about 150 decoded bytes. Streaming cost about 7 dB with weak signals in the documented measurements.

Some modules gate their antenna path behind an external pin and stay roughly 30 dB deaf without it. The Seeed Wio-SX1262 is one of them - on the XIAO ESP32S3 kit that pin is `GPIO38`:

```yaml
wmbus_radio:
  radio_type: SX1262
  rf_sw_pin: GPIO38
```

This is not the same option as `dio2_rf_switch`, and both are needed: DIO2 selects the TX/RX direction inside the chip, `rf_sw_pin` decides whether the module's RF switch conducts at all. Heltec V3/V4/V4-R8 do not need it - they use the `fem_*` pins.

### SX1276

Normal SX1276 boards do not need a TCXO option. Some boards expose a dedicated TCXO enable pin. Configure it explicitly only when your board documentation says so.

Example for LILYGO T3 V3.0 TCXO OLED LoRa32:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

`tcxo_pin`, when configured, is driven HIGH before SX1276 radio initialization.

### CC1101

CC1101 support is experimental and requires explicit opt-in plus dual IRQ wiring. Single-IRQ CC1101 wiring is not supported.

```yaml
wmbus_radio:
  radio_type: CC1101
  cc1101_allow_experimental: true
  gdo0_pin: GPIOxx
  gdo2_pin: GPIOyy
```

Silicon revisions: modules reporting `VERSION=0x14` and `VERSION=0x04` are both accepted. The revision byte is logged, but it does not gate startup — an unrecognised value produces a warning and the receiver still starts. What decides whether the radio is usable is the register self-check (GDO mapping, packet mode, RF profile), and a completely silent SPI bus (`VERSION` reading `0x00` or `0xFF`) still fails setup.

### LR1121

LR1121 support is experimental and requires explicit opt-in. It has decoded real
T1, C1 and S1 traffic on the Waveshare ESP32-S3-LR1121-HF board (SKU 34011).

It has now run for weeks beside four other receivers in one flat, and the
comparison that used to be missing has been made. It is not flattering: over a
12 h window it heard **48 meters against the T-Beam SX1262's 113**, on an
identical 10 cm antenna, the same firmware and the same window. On an attenuator
bench it was the first board to lose the signal entirely, where two SX1262 boards
were still decoding 92.9% of frames. Swapping antennas between the two ruled out
the antenna: the LR1121 lost while holding the better one.

The mechanism is visible in the counters. It has the **highest conversion of any
board here (80% of receiver triggers became frames)** and by far the fewest
triggers. It is the most conservative receiver in the set - it rarely attempts a
marginal start, and rarely wastes one. That is why it hears less: on this
workload, willingness to attempt weak frames is what buys coverage.

Still one board, one flat, one person, and none of it separates the chip from
this driver. The opt-in stays for that reason, not because the driver is
untested.

```yaml
wmbus_radio:
  radio_type: LR1121
  lr1121_allow_experimental: true
  cs_pin: GPIO42
  reset_pin: GPIO39
  irq_pin: GPIO38
  busy_pin: GPIO41
  tcxo_voltage: 3.0v
  payload_length: 255
```

`tcxo_voltage: 3.0v` is measured, not assumed: at `1.8v` the chip reports
`HF_XOSC_START` and never reaches the receive path. `payload_length: 255` is
needed because NES frames are 245 raw bytes.

`HF_XOSC_START` in the boot log is normal and is not a fault — it latches when the
chip enters `STDBY_XOSC`, both calibrations afterwards come back clean, and the
driver explains this in the log.

This board has no USB-serial bridge, so `logger:` needs
`hardware_uart: USB_SERIAL_JTAG` or nothing will appear on the console. It also
has more than one u.FL socket, and only one of them is the sub-GHz path — the
wrong socket gives perfect silence with no error anywhere.

## Documentation

Main documentation lives in `docs/`. Examples live in `examples/`.

Start here:

- [`docs/START_HERE.md`](docs/START_HERE.md)
- [`docs/CONFIG_REFERENCE_MINIMAL.md`](docs/CONFIG_REFERENCE_MINIMAL.md)
- [`docs/RADIO_OPTIONS_MINIMAL.md`](docs/RADIO_OPTIONS_MINIMAL.md)
- [`docs/ON_FRAME.md`](docs/ON_FRAME.md)
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)
- [`docs/DIAGNOSTIC.md`](docs/DIAGNOSTIC.md)
- [`examples/README.md`](examples/README.md)

Deeper notes:

- [`docs/CHIP_SELECTION.md`](docs/CHIP_SELECTION.md)
- [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md)
- [`docs/RX_PIPELINE.md`](docs/RX_PIPELINE.md)
- Release notes: [GitHub Releases](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/releases)

Older detailed README content was moved to [`docs/README_FULL.md`](docs/README_FULL.md).

## Support

This project is intentionally RAW-only and is not a general ESPHome/Home Assistant support desk.

Before opening an issue, read [`SUPPORT.md`](SUPPORT.md).

## Contributing

This repository publishes from a separate development tree, so a contribution does not land through the pull request branch itself — it is ported there first and arrives here as one squashed sync commit. That means your commits will not appear in this repository's history even though the code is yours. Authorship is recorded in two other places instead: the port commit in the development tree names your original commits, and the comment that closes your pull request states explicitly that the code is yours.
