# ESPHome wM-Bus Bridge (RAW-only)

[Polska wersja](README_PL.md)

Stable **wireless M-Bus RF → MQTT** bridge for **SX1262** and **SX1276**.

This project keeps the ESP focused on one job only:
- receive wireless M-Bus frames,
- assemble the telegram,
- publish RAW HEX to MQTT,
- leave meter decoding outside the microcontroller.

It intentionally does **not** decode meters on the ESP.  
It does **not** select drivers, calculate readings, or try to replace `wmbusmeters`.

## Why this project?

Many ESP-based wM-Bus solutions try to do everything on the device.

This one does not.

The goal is a simpler and more stable architecture:
- less CPU and RAM pressure on the ESP,
- fewer firmware-side regressions caused by embedded meter logic,
- easier RF diagnostics,
- easier maintenance,
- final decoding stays on **Home Assistant / Linux / wmbusmeters**, where it belongs.

## Architecture

```text
meter -> SX1262/SX1276 -> ESPHome wmbus_radio -> MQTT HEX -> wmbusmeters / Home Assistant
```

## Best for

- users who want a stable ESP-based radio,
- users who prefer a simple RAW pipeline to MQTT,
- users who want decoding and higher-level diagnostics outside the ESP.

## Quick decision guide

| Scenario | Recommendation |
|---|---|
| House / quiet RF / a few meters / T1 only | `SX1276` is often enough |
| Apartment block / many meters / frequent packets | Prefer `SX1262` |
| Mixed T1 + C1 on one device | Works, but with a real reception cost |
| Best reliability in mixed T1/C1 environment | Use two devices: `T1-only` + `C1-only` |

For more detail see:

- **[`CHIP_SELECTION.md`](CHIP_SELECTION.md)**
- **[`BENCHMARKS.md`](BENCHMARKS.md)**

## Home Assistant add-on

This repo works well with:

- `Kustonium/homeassistant-wmbus-mqtt-bridge`

Raw HEX from MQTT is fed there into `wmbusmeters` via `stdin:hex`.

## Quick start

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s

wmbus_radio:
  radio_type: SX1262   # or SX1276
  # ... SPI + radio pins ...
  telegram_topic: "wmbus_bridge/my_receiver/telegram"  # choose your own MQTT topic
```

Use `on_frame` only when you want extra side effects such as LED blink, extra MQTT topics, or custom per-frame logic.

## Advanced YAML features

Beyond the minimal setup, the component also supports:

- dedicated receiver task stack sizing with `receiver_task_stack_size`
- built-in RAW forwarding with `telegram_topic`
- optional one-meter routing with `target_meter_id` and `target_topic`
- SX1276 ether filtering modes with `sx1276_busy_ether_mode: normal | aggressive | adaptive`
- diagnostic filtering for selected meters with `highlight_meters` and `diagnostic_publish_highlight_only`
- optional log highlighting with `highlight_ansi`, `highlight_tag`, and `highlight_prefix`
- SX1262 boot-time device error clearing with `clear_device_errors_on_boot`
- optional publication of cleared SX1262 device errors with `publish_dev_err_after_clear`
- SX1262 board tuning such as `dio2_rf_switch`, `has_tcxo`, `rx_gain`, `long_gfsk_packets`
- optional Heltec V4 FEM pin configuration with `fem_ctrl_pin`, `fem_en_pin`, and `fem_pa_pin`

The full field list and event details are documented in [`DIAGNOSTIC.md`](DIAGNOSTIC.md).

## What the repo contains

- single public ESPHome component: `wmbus_radio`
- internal implementation for:
  - transceiver handling for `SX1262` and `SX1276`
  - packet assembly
  - internal wireless M-Bus helpers
  - `3of6` decoding
  - DLL CRC handling
  - diagnostic / RX-path support
- examples for:
  - `SX1262 / Heltec V3`
  - `SX1262 / Heltec V4`
  - `SX1262 / XIAO ESP32 S3`
  - `SX1262 / XIAO nRF52840`
  - `SX1276 / Lilygo T3-S3`
  - `SX1276 / Heltec V2`
- board reference assets included with examples:
  - pinmap image for `Heltec V4`
  - board image for `Lilygo T3-S3`
  - board image and reference PDF for `Heltec V2`
- MQTT diagnostics:
  - `boot`
  - `summary`
  - `dropped`
  - `truncated`
  - `rx_path`
  - `meter_window`
  - `busy_ether_changed` (SX1276 adaptive state changes)
  - `suggestion` (throttled diagnostic hints)
  - `dev_err_cleared` (SX1262)

## Documentation map

- **[`DIAGNOSTIC.md`](DIAGNOSTIC.md)** — MQTT fields, YAML options, event meanings, short/long summary windows, and how to read diagnostics
- **[`CHIP_SELECTION.md`](CHIP_SELECTION.md)** — practical SX1276 vs SX1262 selection guide
- **[`BENCHMARKS.md`](BENCHMARKS.md)** — measured benchmark conclusions for `T1-only` and `both`
- **[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)** — symptom-based diagnostic guide
- **[`docs/RELEASE_NOTES.md`](docs/RELEASE_NOTES.md)** — release-level change summary, including adaptive SX1276 behavior, new MQTT diagnostic events, and expanded runtime diagnostics
- **[`docs/PERMISSION.md`](docs/PERMISSION.md)** — short note documenting explicit upstream permission for fork/publication and GPL publication with attribution

## Important diagnostic warning

Do **not** treat `summary` as the same thing as real reception quality.

- `summary` shows parser / decode cleanliness,
- `meter_window` shows real per-meter reception success.

This matters especially on **SX1276**, where `adaptive` is a real window-based algorithm, not a vague auto mode. Once per summary window it checks false-start-like counters, `drop_pct`, T1 symbol errors, and FIFO overruns; when those thresholds indicate a genuinely noisy window, it enables a 5-minute stronger-filtering hold. That can make `summary` look clean while `meter_window` still shows real losses. State transitions are published as `diagnostic_topic/busy_ether_changed`, and diagnostic hints are published as `diagnostic_topic/suggestion`.

## Important note about log language

Documentation is split into separate Polish and English files.

Runtime logging follows a practical mixed policy:

- the most important user-facing `INFO` / `WARN` / `ERROR` messages may be short bilingual `EN / PL`,
- low-level `DEBUG` / `VERBOSE` messages stay in English,
- YAML option names, MQTT event names, and JSON field names stay in English as the stable technical API.

This keeps the logs readable for Polish users without making low-level debugging harder.

## Examples

### SX1262
- `examples/SX1262/Heltec V3/SX1262_V3.yaml`
- `examples/SX1262/Heltec V4/SX1262_full_example_LED.yaml`
- `examples/SX1262/XIAO ESP32 S3/xiao.yaml`
- `examples/SX1262/XIAO nRF52840/XIAO nRF52840.yaml`

### SX1276
- `examples/SX1276/LilygoT3S3/SX1276_T3S3_full_example.yaml`
- `examples/SX1276/HeltecV2/SX1276_Heltec_V2_full_example.yaml`

### Board reference files
- `examples/SX1262/Heltec V4/V4_pinmap.png`
- `examples/SX1276/LilygoT3S3/LilygoT3S3_ver_1_2.png`
- `examples/SX1276/HeltecV2/HeltecV2.png`
- `examples/SX1276/HeltecV2/WIFI_LoRa_32_V2.pdf`

## How this project was built

This project was built in March 2026 over 26 days — from zero to a working release with diagnostics, support for two transceivers, and full documentation.

It started from a practical need: existing solutions did not behave the way I needed in real use. The project was developed iteratively on actual hardware, with a strong focus on stability, observability, and keeping meter decoding outside the ESP device.

AI tools such as Claude and ChatGPT were used during development for drafting code, refactoring, exploring implementation variants, and accelerating iteration. The project direction, requirements, validation, hardware testing, rejection of bad ideas, and architectural decisions remained on my side.

This is documented openly because that is how the project was actually built: not by blindly accepting generated code, but by using AI as a development tool while testing and shaping the system around real-world constraints.

## License

**GPL-3.0-or-later** — see `LICENSE` and `NOTICE`.