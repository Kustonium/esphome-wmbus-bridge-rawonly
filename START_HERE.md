# START_HERE.md

[Polska wersja](START_HERE_PL.md)

This file is the reading path for this repository.

The project has many Markdown files because each one answers a different question. If you are new here, do not read everything at once. Follow this order.

## 0. First: understand what this project is

This is **not** an all-in-one ESPHome meter decoder.

The ESP device is only a Wireless M-Bus RF receiver and MQTT publisher.

```text
meter -> SX1262/SX1276/CC1101 -> ESPHome wmbus_radio -> MQTT -> backend/wmbusmeters -> Home Assistant
```

The ESP does not:

- choose meter drivers,
- decrypt AES,
- create meter value sensors,
- replace `wmbusmeters`.

The ESP does:

- receive T1/C1 frames and experimental S1 frames,
- validate/normalize telegrams,
- publish valid telegram HEX to MQTT,
- publish RF diagnostics.

If you want one ESPHome YAML that directly creates Home Assistant sensors without a backend, this is not the shortest path.

## 1. Choose hardware

Read:

- [`CHIP_SELECTION.md`](CHIP_SELECTION.md)
- [`examples/README.md`](examples/README.md)

Recommended order for beginners:

1. **XIAO ESP32-S3 + Wio-SX1262** — compact, no OLED, good practical target.
2. **Heltec WiFi LoRa 32 V4** — good SX1262 dev board, but V4 needs the correct FEM/RF-switch YAML.
3. **Heltec WiFi LoRa 32 V2 / LilyGo T3-S3 SX1276** — usable SX1276 targets.
4. **CC1101** — experimental, advanced/testing only. Requires `GDO0 + GDO2` and explicit YAML opt-in.

Important: do not copy YAML between Heltec V2/V3/V4 blindly. V4 is not a drop-in replacement for older boards.

## 2. Pick the matching example YAML

Go to [`examples/`](examples/).

Use a `*_clean.yaml` file if you already understand the board.

Use a `*_commented.yaml` file if you want to see why the pins/options are present.

Current example groups:

```text
examples/SX1262/Heltec V4/
examples/SX1262/Heltec V3/
examples/SX1262/XIAO ESP32 S3/
examples/SX1276/HeltecV2/
examples/SX1276/LilygoT3S3/
examples/CC1101/
```

For normal use, keep:

```yaml
diagnostic_mode: normal
```

For known meters, add:

```yaml
highlight_meters:
  - "12345678"
```

This enables per-meter snapshots for the meters that matter.


## 3. Pick the receive mode and frequency

For most installations, start with T1 or T1/C1:

```yaml
wmbus_radio:
  listen_mode: t1
```

```yaml
wmbus_radio:
  listen_mode: both
```

`both` means T1/C1 only. It does not include S1.

S1 is an experimental dedicated receive mode:

```yaml
wmbus_radio:
  listen_mode: s1
```

Default frequencies are mode-aware:

- `t1`, `c1`, `both` -> `868.950 MHz`
- `s1` -> `868.300 MHz`

Use `frequency:` only when you need to override the default, for example for S1 compatibility tests:

```yaml
wmbus_radio:
  listen_mode: s1
  frequency: 868.36
```

If a valid S1 telegram is received, it is forwarded to MQTT like other validated wM-Bus telegrams. Backend decoding still depends on the meter type, driver and encryption key.

## 4. MQTT topics: do not build them manually

Read:

- [`docs/CONFIG_REFERENCE_MINIMAL.md`](docs/CONFIG_REFERENCE_MINIMAL.md)

Recommended configuration:

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

This creates topics like:

```text
wmbus/xiao_s3/telegram
wmbus/xiao_s3/diag/summary
wmbus/xiao_s3/diag/summary_15min
wmbus/xiao_s3/diag/meter_snapshot
wmbus/xiao_s3/diag/boot
```

If `topic_name` is omitted, `esphome.name` is used.

Legacy manual overrides such as `telegram_topic` and `diagnostic_topic` still work, but should not be used in new configs unless you really need them.

## 5. Flash the ESP and check only the RF/MQTT layer first

Before configuring meters in the backend, prove that the ESP receiver works.

Check that MQTT receives telegrams on:

```text
wmbus/<device>/telegram
```

Check diagnostics:

```text
wmbus/<device>/diag/boot
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
```

Do not debug AES keys, meter drivers, or Home Assistant entities before you know that RF/MQTT works.

## 6. Understand diagnostics

Read:

- [`DIAGNOSTIC.md`](DIAGNOSTIC.md)
- [`docs/DIAGNOSTICS_QUICK_REFERENCE.md`](docs/DIAGNOSTICS_QUICK_REFERENCE.md)

For normal users:

```yaml
diagnostic_mode: normal
```

This gives:

- global summary,
- 15-minute summary,
- `meter_snapshot` for `highlight_meters`,
- RF suggestions when enabled by the preset.

Use `debug` or `dev` only for short tests. Do not run full developer diagnostics forever.

## 7. If something does not work, follow the troubleshooting path

Read:

- [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)

The short version:

1. Is anything published to `wmbus/<device>/telegram`?
2. What does `diag/boot` say?
3. What does `diag/summary` say?
4. Are your important meters visible in `meter_snapshot`?
5. Are drops random, CRC-related, false-start-like, or mode-related?
6. Only after RF/MQTT is proven, debug backend decoding.

## 8. Configure the backend

This repository is only the ESP RF receiver.

For decoding and Home Assistant MQTT Discovery use the backend bridge:

- <https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge>

The backend should subscribe to:

```text
wmbus/+/telegram
```

Meter IDs, drivers, AES keys, JSON output, and Home Assistant Discovery belong to the backend side, not to this ESP component.

## 9. Read deeper only when needed

Use these files depending on the question:

| Question | File |
|---|---|
| Which board/radio should I choose? | [`CHIP_SELECTION.md`](CHIP_SELECTION.md) |
| How do diagnostics work? | [`DIAGNOSTIC.md`](DIAGNOSTIC.md) |
| What do drops/CRC/intervals mean? | [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) |
| How does the RX pipeline work internally? | [`docs/RX_PIPELINE.md`](docs/RX_PIPELINE.md) |
| What are the minimal YAML options? | [`docs/CONFIG_REFERENCE_MINIMAL.md`](docs/CONFIG_REFERENCE_MINIMAL.md) |
| What radio options exist? | [`docs/RADIO_OPTIONS_MINIMAL.md`](docs/RADIO_OPTIONS_MINIMAL.md) |
| What changed in releases? | [`docs/RELEASE_NOTES.md`](docs/RELEASE_NOTES.md) |
| How does SX1262 compare with SX1276? | [`BENCHMARKS.md`](BENCHMARKS.md) |
| What is the project scope/support rule? | [`SUPPORT.md`](SUPPORT.md) |

## 10. Where to ask

Use:

- **Issues** for reproducible bugs in this ESP component.
- **Discussions** for questions, hardware tests, working YAML examples, RF results, frequency correction reports.
- Backend repository issues for decoding, meter driver, AES key, JSON, or Home Assistant Discovery problems.

Before asking for help, include:

- board model,
- radio type,
- YAML without secrets,
- ESPHome version,
- full boot log,
- 2-5 minutes of runtime log,
- MQTT topic names,
- `diag/boot`, `summary`, and `meter_snapshot` if available.

No logs, no support.

## 11. The shortest path

```text
1. Read START_HERE.md.
2. Choose board in CHIP_SELECTION.md.
3. Copy matching YAML from examples/.
4. Use diagnostic_mode: normal.
5. Confirm telegrams on MQTT.
6. Confirm diagnostics.
7. Configure backend bridge.
8. Only then debug meter drivers/AES/Home Assistant entities.
```
