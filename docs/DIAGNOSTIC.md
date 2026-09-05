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
wmbus/xiao-s3/diag/config              # retain=true; effective YAML this boot
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
  - "00088888"
  - "03500001"
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
  - "00088888"
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

For SX1262, CC1101 and LR1121:

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

### RSSI fields

`last_rssi` is the level of the most recent frame whose signal could actually be measured. `win_avg_rssi` is the average over the window.

Only frames carrying a real measurement are averaged. A frame for which the radio returned no level is reported as **-127 dBm** ("not measured") and is **kept out of the averages** - otherwise it would drag the statistic down with a value that is not a signal strength. For the same reason `last_rssi` keeps the previous measured reading instead of flipping to the sentinel.

What this means when reading a snapshot:

- `win_avg_rssi: 0` means **no measured samples in the window**, not 0 dBm. The window can still contain frames - packet counters advance independently of the measurement.
- `last_rssi: 0` on a meter that has just appeared means the same: none of its frames carried a measurement yet.
- Treat `-126` and `-127` as no data. The component's own RF heuristics skip them too.

The distribution of these values across meters is a diagnostic tool in its own right - see TROUBLESHOOTING, the section on a narrow RSSI band.

These fields live inside the diagnostic payloads and therefore require a diagnostic mode. `publish_rssi` is a separate, independent option: it publishes the level of each meter's last frame to `wmbus/<topic_name>/rssi/<meter_id>` regardless of `diagnostic_mode`, as a plain integer, and stays silent for frames with no measurement instead of sending a sentinel. Use the diagnostic fields to read the RF picture of the board; use `publish_rssi` to get one signal-strength entity per meter and per board in Home Assistant. See CONFIG_REFERENCE_MINIMAL for the full description.

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

## Receiver triggers: `irq_fired`

`irq_fired` counts the data interrupt itself, before any parsing. It is the
only counter that separates "never heard anything" from "heard it and lost it
downstream", and without it `drop_pct` is misleading: a frame the radio never
attempted is not counted as dropped, so the ratio *improves* as reception gets
worse.

Measured across five boards in one window: the board with the worst conversion
(`total / irq_fired` = 11%) heard the most meters, and the board with the
lowest `drop_pct` (8%) delivered half of what the board with the highest (21%)
did. **Read `irq_fired` next to `dropped`, never `drop_pct` alone.**

It also splits the "no frames" hint in two:

| hint | meaning |
|---|---|
| `NO_DATA` | no triggers at all - antenna, frequency or wiring |
| `RX_NO_MATCH` | triggers but no frames - something IS transmitting; check `listen_mode`, `min_preamble_bits` and the meter's mode |

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

For SX1262, two extra YAML options control device-error handling at boot:

- `clear_device_errors_on_boot: true|false` — when `true`, the component issues a Semtech `ClearDeviceErrors` command on startup so latched errors from a previous power cycle do not persist.
- `publish_dev_err_after_clear: true|false` — when `true`, the SX1262 `dev_err` snapshot (before/after the clear) is published once after boot for diagnostics.

For `SX1276`, the boot sanity report shows whether `tcxo_pin` is configured. Missing `tcxo_pin` is OK for normal SX1276 boards. TCXO variants, such as LILYGO T3 V3.0 TCXO OLED LoRa32, require an explicit board-specific pin, for example `tcxo_pin: GPIO12`.

These reports describe the YAML configuration. They do not auto-detect board wiring.

## MQTT availability

MQTT publishing is intentionally separated from radio reception.

If MQTT is unavailable, received frames are still logged locally and MQTT publishing is skipped with a throttled warning. This helps separate RF problems from transport problems.

If you see `Have data / odebrano dane` locally but nothing reaches the backend, debug MQTT. If you do not see local `Have data` lines, debug RF/board configuration first.

## Boot configuration report

Every boot, and again on the periodic boot-log line, the component prints the
**effective configuration of this board**, one option per line, marked so a
reader can tell a choice from an inheritance:

```text
[I][wmbus] Configuration / konfiguracja (SX1262):
[I][wmbus]   [core]
[I][wmbus]   radio_type: SX1262 (required)
[I][wmbus]   listen_mode: t1 (CHANGED, default: both)
[I][wmbus]   receiver_task_stack_size: 6144 (CHANGED, default: 3072)
[I][wmbus]   [pins]
[I][wmbus]   cs_pin: GPIO41 (set)
[I][wmbus]   rf_sw_pin: GPIO38 (set)
[I][wmbus]   [sx1262]
[I][wmbus]   has_tcxo: true (CHANGED, default: false)
[I][wmbus]   rx_gain: boosted (default)
```

Markers:

| marker | meaning |
|---|---|
| `(required)` | must be given; `radio_type` |
| `(set)` | no schema default exists — a pin, a topic name |
| `(default)` | present, and equal to the schema default |
| `(CHANGED, default: X)` | you set it, and it differs from `X` |
| `not set (default: X)` | absent; `X` applies |

The list is built at compile time from the schema, so a default can never drift
away from what the driver actually uses. Only options that apply to the selected
radio are listed — `has_tcxo` never appears for `CC1101`.

Why it exists: the log used to show a handful of hand-picked checks, so anything
outside that list was invisible and a misconfigured board still looked healthy.
With the report, a support question can be answered from the log alone, and
"I did not change that" becomes checkable.

## Per-radio sanity checks

Beyond the report, each radio logs the checks whose failure is *silent* — the
board initializes, the log looks fine, and reception is bad or absent:

- **SX1262** — `has_tcxo` (a TCXO board without it may receive nothing),
  `dio2_rf_switch`, `long_gfsk_packets` (long T1 frames), `rx_gain`, and
  **`rf_sw_pin`**: required on XIAO ESP32-S3 + Wio-SX1262 (`GPIO38`), and without
  it the board is roughly 30 dB deaf while looking perfectly healthy.
- **SX1276** — `tcxo_pin` (LilyGO T3 V3.0 TCXO uses `GPIO12`).
- **CC1101** — the experimental gate plus `gdo0_pin`/`gdo2_pin`, so dual-IRQ
  wiring is confirmed rather than inferred.
- **LR1121** — `tcxo_voltage`, `tcxo_startup_ticks`, `rx_bandwidth` against the
  required `2*fdev + bitrate`, `payload_length`, `rx_boosted`, and the
  calibration stages behind the known `HF_XOSC_START` transient.
