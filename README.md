# ESPHome wM-Bus Bridge (RAW-only)

[Polska wersja](README_PL.md)

Minimal **RF → MQTT** bridge where the ESP acts only as a wM-Bus radio receiver.

## Project idea

This component:

- receives wM-Bus frames from **SX1262** or **SX1276**,
- detects **T1 / C1** and frame **A / B**,
- assembles the telegram and publishes it as **HEX** to MQTT,
- keeps **all meter decoding outside the ESP**.

This repo intentionally **does not decode meters on the ESP**. It does not select drivers, calculate values, or try to replace `wmbusmeters`.

## Why RAW-only?

This design keeps the radio path and the meter-decoding path separate.

Practical benefits:

- less CPU/RAM pressure on the ESP,
- fewer firmware-side regressions caused by meter-driver logic,
- easier RF diagnostics,
- final decoding stays on **Home Assistant / Linux / wmbusmeters**, where it belongs.

## Architecture

```text
meter -> SX1262/SX1276 -> ESPHome wmbus_radio -> MQTT HEX -> wmbusmeters / Home Assistant
```

## Who is this for?

For people who want:

- a stable ESP-based radio,
- a simple RAW pipeline to MQTT,
- final decoding and higher-level diagnostics on **Home Assistant / Linux / wmbusmeters**.

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

## Important diagnostic warning

Do **not** treat `summary` as the same thing as real reception quality.

- `summary` shows parser / decode cleanliness,
- `meter_window` shows real per-meter reception success.

This matters especially on **SX1276**, where `adaptive` is a real window-based algorithm, not a vague auto mode. Once per summary window it checks false-start-like counters, `drop_pct`, T1 symbol errors, and FIFO overruns; when those thresholds indicate a genuinely noisy window, it enables a 5-minute stronger-filtering hold. That can make `summary` look clean while `meter_window` still shows real losses. State transitions are published as `diagnostic_topic/busy_ether_changed`, and diagnostic hints are published as `diagnostic_topic/suggestion`.

## What the repo contains

- `wmbus_radio` component,
- examples for:
  - `SX1262 / Heltec V4`
  - `SX1276 / Lilygo T3-S3`
  - `SX1276 / Heltec V2`
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

## Quick start

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s

wmbus_radio:
  radio_type: SX1262   # or SX1276
  # ... SPI + radio pins ...
  telegram_topic: "wmbus_bridge/telegram"   # built-in RAW publish for wmbusmeters
```

Use `on_frame` only when you want extra side effects such as LED blink, extra MQTT topics, or custom per-frame logic.

## Documentation map

- **[`DIAGNOSTIC.md`](DIAGNOSTIC.md)** — MQTT fields, YAML options, event meanings, short/long summary windows, and how to read diagnostics
- **[`CHIP_SELECTION.md`](CHIP_SELECTION.md)** — practical SX1276 vs SX1262 selection guide
- **[`BENCHMARKS.md`](BENCHMARKS.md)** — measured benchmark conclusions for `T1-only` and `both`
- **[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)** — symptom-based diagnostic guide

## Important note about log language

Documentation is split into separate Polish and English files.

Runtime logging follows a practical mixed policy:

- the most important user-facing `INFO` / `WARN` / `ERROR` messages may be short bilingual `EN / PL`,
- low-level `DEBUG` / `VERBOSE` messages stay in English,
- YAML option names, MQTT event names, and JSON field names stay in English as the stable technical API.

This keeps the logs readable for Polish users without making low-level debugging harder.

## Examples

- `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml`
- `examples/SX1276/LilygoT3S3/SX1276_T3S3_full_example.yaml`
- `examples/SX1276/HeltecV2/SX1276_Heltec_V2_full_example.yaml`

## Home Assistant add-on

This repo works well with:

- `Kustonium/homeassistant-wmbus-mqtt-bridge`

Raw HEX from MQTT is fed there into `wmbusmeters` via `stdin:hex`.

## How this project was built

This project was built in March 2026 over 26 days — from zero to a working release with diagnostics, support for two transceivers, and full documentation.

It started from a practical need: existing solutions did not behave the way I needed in real use. The project was developed iteratively on actual hardware, with a strong focus on stability, observability, and keeping meter decoding outside the ESP device.

AI tools (Claude and ChatGPT) were used during development for drafting code, refactoring, exploring implementation variants, and accelerating iteration. The project direction, requirements, validation, hardware testing, rejection of bad ideas, and architectural decisions remained on my side.

This is documented openly because it reflects how the project was actually built: not by blindly accepting generated code, but by using AI as a development tool while testing and shaping the system around real-world constraints.

## License

**GPL-3.0-or-later** — see `LICENSE` and `NOTICE`.
