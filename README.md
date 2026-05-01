# ESPHome wM-Bus Bridge RAW-only

[Polska wersja](README_PL.md)

RAW-only wireless M-Bus radio bridge for ESPHome.

The ESP receives and validates wM-Bus telegrams, then publishes validated RAW HEX to MQTT. Meter decoding stays outside the ESP, for example in Home Assistant / Linux / `wmbusmeters`.

```text
meter -> SX1262/SX1276 -> ESPHome wmbus_radio -> MQTT HEX -> wmbusmeters / Home Assistant
```

## Design rule

The ESP is a radio bridge, not a meter decoder.

It does not:
- select `wmbusmeters` drivers,
- decrypt AES payloads,
- create meter value sensors,
- replace `wmbusmeters`.

It does:
- receive T1/C1 frames,
- validate/normalize telegrams,
- publish valid telegram HEX to MQTT,
- publish RF diagnostics.

## MQTT topic scheme

The recommended topic scheme is:

```text
wmbus/<device>/telegram
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
wmbus/<device>/diag/boot
```

The Home Assistant bridge add-on should subscribe to:

```text
wmbus/+/telegram
```

Do not manually build topic paths in normal YAML. Use `topic_name`, or omit it and let the component use `esphome.name`.

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

This generates:

```text
wmbus/xiao_s3/telegram
wmbus/xiao_s3/diag/...
```

If `topic_name` is omitted, `esphome.name` is used. `friendly_name` is not used for topics because it may contain spaces, uppercase characters or non-ASCII characters.

`topic_name` accepts only letters, digits, `_` and `-`. Do not include `wmbus/`, `/`, `+`, `#` or spaces.

Legacy manual overrides still work:

```yaml
telegram_topic: "..."
diagnostic_topic: "..."
```

but they are intended only for compatibility and produce a bilingual warning.

## Quick start

Clean minimal example:

```yaml
substitutions:
  devicename: esphome-wmbus-xiao-s3
  friendly_name: "wMBus Bridge XIAO S3"

esphome:
  name: ${devicename}
  friendly_name: ${friendly_name}

external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s

wmbus_radio:
  radio_type: SX1262
  listen_mode: t1

  # Optional. If omitted, esphome.name is used.
  # topic_name: "${devicename}"

  diagnostic_mode: normal

  # Optional. In diagnostic_mode: normal this enables meter_snapshot
  # for these meters.
  highlight_meters:
    - "12345678"
    - "11335577"
    - "22446688"

  # ... SPI/radio pins go here ...
```

## Diagnostic modes

Use presets, not a pile of individual flags:

| Mode | Meaning |
|---|---|
| `off` | no MQTT diagnostics |
| `low` | global `summary` + hint |
| `normal` | `summary` + `summary_15min` + `meter_snapshot` for `highlight_meters` |
| `debug` | `normal` + drop/RX-path events |
| `dev` | full developer diagnostics, including raw/debug payloads |

Old modes remain accepted as deprecated aliases:
- `medium` -> `normal`
- `full` -> `dev`
- `raw` -> `dev`

Old detailed options such as `diagnostic_publish_summary_highlight_meters` and `diagnostic_publish_highlight_only` still compile for compatibility, but they are deprecated/advanced. Use the presets first.

`diagnostic_publish_highlight_only` was a confusing name. It filters detailed diagnostic events to `highlight_meters`; it does not enable meter statistics. The clearer name is:

```yaml
diagnostic_events_highlight_only: true
```

## Per-meter statistics

For normal use:

```yaml
diagnostic_mode: normal
highlight_meters:
  - "00089907"
  - "03534159"
```

This publishes a combined snapshot on:

```text
wmbus/<device>/diag/meter_snapshot
```

For advanced use:

```yaml
diagnostic_meter_stats: highlighted
```

or:

```yaml
diagnostic_meter_stats: all
```

Use `all` only for development or controlled testing in dense RF environments.

## `listen_mode_filter_after_parse`

Default:

```yaml
listen_mode_filter_after_parse: false
```

This is the conservative/stable behavior. It is recommended when meters are nearby and reception is already good.

Experimental mode:

```yaml
listen_mode_filter_after_parse: true
```

This filters `listen_mode` after frame parsing/CRC/fallback has determined the final T1/C1 mode. It may help when meters are farther away, behind walls, or when frames are partially lost.

It may also increase:
- `false_start_like`,
- `payload_size_unknown`,
- `t1_decode3of6` drops.

Compare this option using `meter_snapshot` for the meters that matter, not only global `drop_pct`.

## Radio notes

- SX1262 is preferred for dense RF, frequent packets and long T1 frames.
- SX1276 can work well, especially in T1-only and quieter environments.
- For mixed T1/C1 environments, two dedicated receivers are usually better than one receiver in `both`.

`busy_ether_state` is SX1276-only. For SX1262 and CC1101 it is reported as:

```json
"busy_ether_state": "n/a"
```

## CC1101

CC1101 support exists in the component but is still experimental and intentionally not shown in the public examples. It requires explicit opt-in in YAML and validated GDO0/GDO2 wiring.

## Documentation

- [`DIAGNOSTIC.md`](DIAGNOSTIC.md)
- [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)
- [`CHIP_SELECTION.md`](CHIP_SELECTION.md)
- [`BENCHMARKS.md`](BENCHMARKS.md)
- [`docs/RX_PIPELINE.md`](docs/RX_PIPELINE.md)

## Support rule

No logs, no support.

Before asking for help provide:
- YAML without secrets,
- full boot log,
- 2-5 minutes of runtime log,
- radio module and ESP board model,
- wiring photo if using an external radio.
