# Diagnostics

[Polska wersja](DIAGNOSTIC_PL.md)

## MQTT topics

With:

```yaml
esphome:
  name: xiao-s3

wmbus_radio:
  diagnostic_mode: normal
```

the component generates:

```text
wmbus/xiao-s3/telegram
wmbus/xiao-s3/diag/summary
wmbus/xiao-s3/diag/summary_15min
wmbus/xiao-s3/diag/meter_snapshot
wmbus/xiao-s3/diag/boot
```

Use `topic_name` to override only the device part:

```yaml
topic_name: "xiao_s3"
```

Do not include `wmbus/` in `topic_name`.

Legacy `telegram_topic` and `diagnostic_topic` still work, but they are manual overrides and produce a bilingual warning.

## Diagnostic modes

| Mode | MQTT output |
|---|---|
| `off` | no MQTT diagnostics |
| `low` | global summary + hint |
| `normal` | global summary + 15-min summary + meter snapshot for `highlight_meters` |
| `debug` | `normal` + drop/RX-path events |
| `dev` | full developer diagnostics including raw/debug payloads |

Deprecated aliases:
- `medium` -> `normal`
- `full` -> `dev`
- `raw` -> `dev`

## Recommended normal diagnostics

```yaml
diagnostic_mode: normal
highlight_meters:
  - "00089907"
  - "03534159"
```

This publishes:

```text
wmbus/<topic_name>/diag/meter_snapshot
```

and tracks per-meter windows for the IDs from `highlight_meters`.

## Event filtering

New clear name:

```yaml
diagnostic_events_highlight_only: true
```

This limits detailed diagnostic events to meters from `highlight_meters`.

Deprecated old name:

```yaml
diagnostic_publish_highlight_only: true
```

It still works, but it is confusing and will produce a warning. It does **not** enable per-meter statistics.

## Per-meter statistics

Recommended:

```yaml
diagnostic_mode: normal
highlight_meters:
  - "00089907"
```

Advanced:

```yaml
diagnostic_meter_stats: highlighted
```

or:

```yaml
diagnostic_meter_stats: all
```

`all` tracks every decoded meter ID and should be used only for development or controlled tests.

## `summary`

Main topic:

```text
wmbus/<topic_name>/diag/summary
```

Important fields:
- `total` — candidates processed by the validated frame path,
- `ok` — valid frames,
- `dropped` — rejected frames,
- `crc_failed` — DLL CRC failures,
- `drop_pct` — global percentage, useful but not decisive,
- `dropped_by_reason`,
- `dropped_by_stage`,
- `rx_path`,
- `hint_code`,
- `busy_ether_state`.

For SX1262 and CC1101:

```json
"busy_ether_state": "n/a"
```

SX1276 may report `normal`, `aggressive`, `adaptive_active` or `adaptive_passive`.

## `meter_snapshot`

Main topic:

```text
wmbus/<topic_name>/diag/meter_snapshot
```

This is the best metric for A/B tests.

Compare:
- `count_window`,
- `win_avg_interval_s`,
- `win_interval_n`.

Do not judge RF changes only by global `drop_pct`. A more aggressive mode can increase `drop_pct` but still recover more frames for meters that matter.

## `listen_mode_filter_after_parse`

Default:

```yaml
listen_mode_filter_after_parse: false
```

Conservative mode. Recommended when meters are nearby and stable.

Experimental:

```yaml
listen_mode_filter_after_parse: true
```

May help for distant meters, walls or partially lost frames. Usually increases:
- `false_start_like`,
- `payload_size_unknown`,
- `t1_decode3of6`.

Judge it by `meter_snapshot`, not by summary alone.

## Legacy detailed options

These still compile for compatibility, but are deprecated/advanced:

```yaml
diagnostic_publish_summary
diagnostic_publish_summary_15min
diagnostic_publish_summary_60min
diagnostic_publish_drop_events
diagnostic_publish_rx_path_events
diagnostic_publish_highlight_only
diagnostic_publish_summary_highlight_meters
diagnostic_publish_raw
diagnostic_verbose
```

Use `diagnostic_mode` presets first.

## Boot sanity reports

Startup logs include radio sanity information before normal troubleshooting starts.

For `SX1262`, the boot sanity report shows the effective YAML values for:

- `has_tcxo`
- `dio2_rf_switch`
- `long_gfsk_packets`
- `rx_gain`

Risky settings are printed as warnings. They do not block startup, because some users intentionally test incomplete or unusual configurations.

For `SX1276`, the boot sanity report shows whether `tcxo_pin` is configured. Missing `tcxo_pin` is OK for normal SX1276 boards. TCXO variants, such as LILYGO T3 V3.0 TCXO OLED LoRa32, require an explicit board-specific pin, for example `tcxo_pin: GPIO12`.

These reports describe the YAML configuration. They do not auto-detect board wiring.

## MQTT availability

MQTT publishing is intentionally separated from radio reception.

If MQTT is unavailable, received frames are still logged locally and MQTT publishing is skipped with a throttled warning. This helps separate RF problems from transport problems.

If you see `Have data / odebrano dane` locally but nothing reaches the backend, debug MQTT. If you do not see local `Have data` lines, debug RF/board configuration first.
