# DIAGNOSTIC.md

[Polska wersja](DIAGNOSTIC_PL.md)

Detailed description of diagnostics, MQTT fields, and YAML options for `wmbus_radio`.

See also:

- [`CHIP_SELECTION.md`](CHIP_SELECTION.md)
- [`BENCHMARKS.md`](BENCHMARKS.md)
- [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)

---

## 1. Important note about message language

Documentation is split into separate Polish and English files.

Runtime logging follows a practical policy:

- important user-facing `INFO` / `WARN` / `ERROR` messages may be short bilingual `EN / PL`,
- low-level `DEBUG` / `VERBOSE` messages stay in English,
- YAML option names, MQTT event names, and JSON field names remain English-only as the stable technical API.

Treat the following as the stable API of the project:

- YAML option names,
- JSON field names published to MQTT,
- event semantics (`boot`, `summary`, `dropped`, `truncated`, `rx_path`, `meter_window`, `busy_ether_changed`, `suggestion`, `dev_err_cleared`).

Comments in examples and documentation explain how to interpret these fields.

---

## 2. What counts as a “telegram” in this repo

There are two different data types in this repo.

### A. RAW telegram for downstream decoding

This is the plain HEX payload published for example to:

- `wmbus_bridge/telegram`

This is **not JSON** and has no descriptive fields.

Most commonly you use:

- `telegram_topic` — the preferred built-in RAW MQTT publish path
- `frame->as_hex()` — HEX after DLL CRC removal, best for `wmbusmeters` when you need custom `on_frame` logic
- `frame->as_rtlwmbus()` — rtl-wmbus style text for optional extra topics

### B. Diagnostic telegram

This is JSON published to `diagnostic_topic`.

Events:

- `boot`
- `summary`
- `dropped`
- `truncated`
- `rx_path`
- `meter_window`
- `busy_ether_changed`
- `suggestion`
- `dev_err_cleared`

---

## 3. All YAML options

### 3.1 Required

| Key | Description |
|---|---|
| `radio_type` | `SX1262` or `SX1276` |
| `reset_pin` | Radio RESET pin |
| `irq_pin` | Radio IRQ / DIO pin used by the driver |
| `spi_id`, `clk_pin`, `mosi_pin`, `miso_pin`, `cs_pin` | SPI configuration |

### 3.2 General

| Key | Default | Meaning |
|---|---|---|
| `busy_pin` | none | BUSY pin for SX1262. Do not use on SX1276. |
| `listen_mode` | `both` | `t1`, `c1`, `both`. Filters what the component counts and forwards. |
| `on_frame` | none | Callback for every valid frame. |

### How `listen_mode` works

- `t1` — T1 only
- `c1` — C1 only
- `both` — T1 + C1

The filter works **before** further processing and **before diagnostic counters**.

This matters:

- `summary.total` does not count frames rejected by `listen_mode`
- `dropped_by_stage.link_mode` does not mean “wrong YAML mode”, it means mode-detection issues

### Which mode to use when

- **Poland, most water/heat meters on T1** → start with `t1`
- **You know the meter transmits C1** → use `c1`
- **Mixed environment and testing** → use `both`

Note: on **SX1276**, `both` has a real reception cost.

---

### 3.3 Built-in RAW publishing

| Key | Default | Meaning |
|---|---|---|
| `telegram_topic` | none | Main topic for all RAW telegrams. |
| `target_meter_id` | none | Selected meter ID for a dedicated topic. |
| `target_topic` | none | Dedicated topic for `target_meter_id`. |
| `target_log` | `true` | Whether to log packets for the selected meter. |

---

### 3.4 SX1262-specific options

| Key | Default | Meaning |
|---|---|---|
| `dio2_rf_switch` | `true` | Use DIO2 as RF switch. |
| `rf_switch` | none | Alias for `dio2_rf_switch`. |
| `has_tcxo` | `false` | Enable TCXO instead of plain crystal. |
| `rx_gain` | `boosted` | `boosted` or `power_saving`. |
| `long_gfsk_packets` | `false` | Longer GFSK packet handling for harder frames. |
| `fem_ctrl_pin` | none | FEM RX/TX control pin. |
| `fem_en_pin` | none | FEM / LNA enable pin. |
| `fem_pa_pin` | none | PA enable pin. |
| `clear_device_errors_on_boot` | `false` | Clear latched device errors once at boot. |
| `publish_dev_err_after_clear` | `false` | Publish `dev_err` before and after clear. Requires `diagnostic_topic`. |

### When to enable `long_gfsk_packets`

Use it when you have longer telegrams or a rough RF environment and the normal mode is not stable enough.

---

### 3.5 SX1276-specific options

| Key | Default | Meaning |
|---|---|---|
| `sx1276_busy_ether_mode` | `adaptive` | `normal`, `aggressive`, `adaptive` |

### What `sx1276_busy_ether_mode` does

- `normal`  
  No extra filtering. The radio tries to read everything.

- `adaptive`  
  Default mode. The component evaluates the current short diagnostic window and enables stronger filtering only when the window looks like a real busy-ether case.

- `aggressive`  
  Stronger filtering is always on.

### How `adaptive` actually works

`adaptive` is not a generic auto preset. On **SX1276** it is a concrete algorithm evaluated **once per short `summary` window, before counters are reset**.

It builds a local false-start-like score from:

- `preamble_read_failed`
- `payload_size_unknown`
- `weak_start_aborted`
- `probe_start_aborted`
- `raw_drain_skipped_weak`

Then it checks whether the current window matches one of these trigger cases:

- any `fifo_overrun` (always critical — hardware overflow)
- `false_start_like >= 80` **and** `drop_pct >= 10`
- `preamble_read_failed >= 25` **and** `probe_start_aborted >= 20` **and** `drop_pct >= 10`
- `drop_pct >= 20` **and** `false_start_like >= 30`
- `t1_sym_invalid_pct >= 5` **and** `false_start_like >= 20` **and** `drop_pct >= 10`

If triggered, `adaptive` enters a **5-minute hold** where SX1276 uses stricter weak-start / probe-start / raw-drain decisions. If a later window is still noisy, that 5-minute hold is extended again.

If the hold changes state, the repo publishes:

- `diagnostic_topic/busy_ether_changed` with `state=adaptive_active`
- `diagnostic_topic/busy_ether_changed` with `state=adaptive_passive`

This is why `summary` can look cleaner on SX1276: some bad starts are rejected before they ever become full decode attempts.

### Practical recommendation

**Keep `adaptive` if:**

- you are in an apartment block or other dense RF area,
- you do not yet know how clean the ether is,
- you see lots of `false_start_like`, `probe_start_aborted`, `preamble_read_failed`,
- `meter_window` shows real losses despite a clean-looking `summary`.

**Switch to `normal` if:**

- you only have a few meters,
- intervals are long,
- the ether is quiet,
- `summary` and `meter_window` look good without signs of congestion.

Use **`aggressive` only deliberately**, usually for testing or very heavy RF congestion.

It can cut legitimate but weaker meters.

---

### 3.6 Diagnostics

| Key | Default | Meaning |
|---|---|---|
| `diagnostic_topic` | `wmbus/diag` | Base diagnostics topic. |
| `diagnostic_verbose` | `true` | Also log drop/truncate details to serial/API. |
| `diagnostic_publish_summary` | `true` | Publish periodic `summary`. |
| `diagnostic_publish_drop_events` | `true` | Publish `dropped` / `truncated` events. |
| `diagnostic_publish_rx_path_events` | `true` | Publish live `rx_path` events. |
| `diagnostic_publish_highlight_only` | `false` | Limit per-packet diagnostics to IDs in `highlight_meters`. |
| `diagnostic_publish_summary_highlight_meters` | `false` | After each `summary_15min` and `summary_60min`, publish a `meter_window` snapshot for every ID in `highlight_meters`. Read-only — does not reset per-meter counters. |
| `diagnostic_publish_raw` | `true` | Include `raw(hex)` in `dropped` / `truncated` events. |
| `diagnostic_summary_interval` | `60s` | How often to publish the short-window `summary`. |
| `diagnostic_publish_summary_15min` / `diagnostic_publish_summary_60min` | disabled | Optional fixed extra `summary` windows: 15 minutes and 60 minutes, enabled independently with booleans. Published with the same payload format plus `interval_s`, `uptime_ms`, and `listen_mode`. Topics are separate: `/summary`, `/summary_15min`, and `/summary_60min` under `diagnostic_topic`. Note: `busy_ether_state` is present only in the short `/summary`, not in `/summary_15min` or `/summary_60min`. |

### Derived diagnostic topics without dedicated YAML keys

The following topics exist automatically under `diagnostic_topic` when their feature is active:

- `/busy_ether_changed` — SX1276 adaptive state changes (`adaptive_active` / `adaptive_passive`)
- `/suggestion` — throttled actionable hints derived from the current diagnostic window

### How the `suggestion` topic works

`diagnostic_topic/suggestion` is a derived helper topic. It is **not retained** and is **throttled to once per hour per suggestion code**.

Current payload fields are:

- `event` = `suggestion`
- `chip`
- `code`
- `yaml_key`
- `suggested_value`
- `yaml_snippet` — ready-to-paste YAML fragment, can be copied directly into configuration
- `hint_en`
- `hint_pl`

The current implementation can publish suggestions such as:

- `NO_METERS_DETECTED` — no frames received at all
- `ADD_HIGHLIGHT_METERS` — frames exist, but `highlight_meters` is still empty
- `ENABLE_RX_PATH_EVENTS` — SX1276 shows many false starts
- `ENABLE_DROP_EVENTS_RAW` — many drops at weak RSSI, suggesting deeper packet inspection
- `SX1262_SYMBOL_ERRORS` — SX1262 shows T1 symbol errors, suggesting `cpu_frequency: 160MHz`
- `QUIET_ETHER_ADAPTIVE_IDLE` — SX1276 in `adaptive` has stayed quiet long enough that `normal` may be worth testing

### Sensible daily-use profile

```yaml
diagnostic_verbose: false
diagnostic_publish_summary: true
diagnostic_publish_drop_events: true
diagnostic_publish_rx_path_events: false
diagnostic_publish_highlight_only: true
diagnostic_publish_raw: false
```

---

### 3.7 Highlighting and per-meter stats

| Key | Default | Meaning |
|---|---|---|
| `highlight_meters` | `[]` | Meter IDs to highlight and track. |
| `highlight_ansi` | `false` | ANSI color in logs. |
| `highlight_tag` | `wmbus_user` | Dedicated log tag for highlighted meters. |
| `highlight_prefix` | `★ ` | Log prefix. |

For every ID in `highlight_meters`, the component publishes `meter_window`.

If a meter sends **both T1 and C1 under the same ID**, stats remain separate.

---

## 4. Methods available in `on_frame`

| Method | Returns | Meaning |
|---|---|---|
| `frame->as_hex()` | `std::string` | HEX without DLL CRC, usually for `wmbusmeters` |
| `frame->as_raw()` | `std::vector<uint8_t>` | Raw frame bytes |
| `frame->as_rtlwmbus()` | `std::string` | rtl-wmbus text format |
| `frame->rssi()` | `int8_t` | RSSI in dBm |
| `frame->link_mode()` | `LinkMode` | `T1` or `C1` |
| `frame->format()` | `std::string` | `"A"` or `"B"` |

Example:

```yaml
wmbus_radio:
  telegram_topic: "wmbus_bridge/telegram"

  on_frame:
    then:
      - mqtt.publish:
          topic: "wmbus_bridge/rtlwmbus"
          payload: !lambda return frame->as_rtlwmbus();
```

Use `telegram_topic` for the main RAW stream. Keep `on_frame` for optional extras such as LED blink, RSSI topics, or alternate formats.

---

## 5. MQTT topics

### 5.1 RAW telegram

- `telegram_topic` — preferred for the main RAW stream
- a topic published manually from `on_frame` — only when you need custom logic or extra formats

Most common example:

- `wmbus_bridge/telegram`

### 5.2 Diagnostics

Base topic:

- `diagnostic_topic`

Default:

- `wmbus/diag`

Summary topics are separate:

- short summary: `{diagnostic_topic}/summary`
- 15-minute summary: `{diagnostic_topic}/summary_15min`
- 60-minute summary: `{diagnostic_topic}/summary_60min`

Per-packet events such as `dropped`, `truncated`, `rx_path`, and boot/device-error events continue to use the base `diagnostic_topic`.

### 5.3 `meter_window`

For meter windows the topic is extended to:

- `{diagnostic_topic}/meter/{meter_id}/{mode}/window/{trigger}`

Example:

- `wmbus/diag/meter/00089907/T1/window/count`

---

## 6. Event `boot`

Published once after boot.

Example:

```json
{"event":"boot","radio":"SX1262","listen_mode":"T1+C1 (both, 3:1 bias)","uptime_ms":8120}
```

| Field | Meaning |
|---|---|
| `event` | always `boot` |
| `radio` | radio name, e.g. `SX1262`, `SX1276` |
| `listen_mode` | active listen mode as text |
| `uptime_ms` | uptime in milliseconds at publish time |

How to read it:

- mostly confirms that the device booted and in which mode it is running

---

## 7. Event `summary`

This is the main aggregate payload.

Example:

```json
{
  "event":"summary",
  "interval_s":60,
  "uptime_ms":812000,
  "listen_mode":"T1 only",
  "total":25,
  "ok":22,
  "truncated":1,
  "dropped":2,
  "crc_failed":1,
  "crc_fail_pct":4,
  "drop_pct":8,
  "trunc_pct":4,
  "avg_ok_rssi":-70,
  "avg_drop_rssi":-88
}
```

### 7.1 Top-level fields

| Field | Meaning |
|---|---|
| `event` | always `summary` |
| `interval_s` | actual elapsed window in seconds for this summary publication |
| `uptime_ms` | device uptime at publication time |
| `listen_mode` | configured receiver mode: `T1 only`, `C1 only`, or `T1+C1 (both, 3:1 bias)` |
| `total` | number of frames that passed `listen_mode` and entered the parser |
| `ok` | frames successfully assembled to the end |
| `truncated` | cut / incomplete frames |
| `dropped` | frames rejected as bad |
| `crc_failed` | how many drops were DLL CRC failures |
| `crc_fail_pct` | percentage of `crc_failed` against `total` |
| `drop_pct` | percentage of `dropped` against `total` |
| `trunc_pct` | percentage of `truncated` against `total` |
| `avg_ok_rssi` | average RSSI of valid frames |
| `avg_drop_rssi` | average RSSI of dropped frames |

### 7.2 Sections `t1` and `c1`

Both sections use almost the same fields.

#### `t1`

| Field | Meaning |
|---|---|
| `total` | number of T1 frames in the window |
| `ok` | valid T1 frames |
| `dropped` | dropped T1 frames |
| `per_pct` | drop percentage inside T1 |
| `crc_failed` | T1 frames rejected by DLL CRC |
| `crc_pct` | T1 CRC-failure percentage |
| `avg_ok_rssi` | average RSSI of valid T1 frames |
| `avg_drop_rssi` | average RSSI of dropped T1 frames |
| `sym_total` | number of T1 3-of-6 symbols counted |
| `sym_invalid` | number of invalid T1 3-of-6 symbols |
| `sym_invalid_pct` | percentage of invalid T1 3-of-6 symbols |

#### `c1`

| Field | Meaning |
|---|---|
| `total` | number of C1 frames in the window |
| `ok` | valid C1 frames |
| `dropped` | dropped C1 frames |
| `per_pct` | drop percentage inside C1 |
| `crc_failed` | C1 frames rejected by DLL CRC |
| `crc_pct` | C1 CRC-failure percentage |
| `avg_ok_rssi` | average RSSI of valid C1 frames |
| `avg_drop_rssi` | average RSSI of dropped C1 frames |

### 7.3 `dropped_by_reason`

| Field | Meaning |
|---|---|
| `too_short` | not enough data even for a sensible attempt |
| `decode_failed` | the decoder could not assemble a frame |
| `dll_crc_failed` | payload looked sane but DLL CRC failed |
| `unknown_preamble` | preamble not recognized |
| `l_field_invalid` | L-field does not match the frame |
| `unknown_link_mode` | link mode could not be determined correctly |
| `other` | everything else |

### 7.4 `dropped_by_stage`

| Field | Meaning |
|---|---|
| `precheck` | failure before the real parser |
| `t1_decode3of6` | T1 3-of-6 decode failure |
| `t1_l_field` | T1 L-field failure |
| `t1_length_check` | inconsistent T1 length |
| `c1_precheck` | early C1 failure |
| `c1_preamble` | C1 preamble issue |
| `c1_suffix` | C1 suffix issue |
| `c1_l_field` | C1 L-field failure |
| `c1_length_check` | inconsistent C1 length |
| `dll_crc_first` / `dll_crc_mid` / `dll_crc_final` | where DLL CRC failed for block-based formats |
| `dll_crc_b1` / `dll_crc_b2` | DLL CRC failure in B1/B2 |
| `link_mode` | link mode recognition problem |
| `other` | everything else |

### 7.5 `rx_path`

This section describes what happened **on the receive path**, before the parser had a finished frame.

| Field | Meaning |
|---|---|
| `irq_timeout` | timeout while waiting for IRQ |
| `preamble_read_failed` | preamble could not be read reliably |
| `preamble_retry_recovered` | preamble first looked bad, but retry recovered it |
| `t1_header_read_failed` | T1 header could not be read reliably |
| `payload_size_unknown` | size could not be determined after frame start |
| `raw_drain_attempted` | how many times the driver tried blind raw-drain |
| `raw_drain_recovered` | how many of those attempts recovered a frame |
| `raw_drain_recovery_pct` | success rate of raw-drain recovery |
| `raw_drain_bytes` | extra bytes read that way |
| `payload_read_failed` | payload read could not be completed |
| `queue_send_failed` | queue to worker task was full or busy |
| `fifo_overrun` | FIFO / RX-path overrun |
| `weak_start_aborted` | very weak frame start was cut |
| `probe_start_aborted` | weak start was cut already at T1 probe stage |
| `raw_drain_skipped_weak` | raw-drain was skipped because start was too weak |
| `false_start_like` | combined counter of garbage-like starts |

#### `probe_abort_rssi` and `weak_abort_rssi`

These are RSSI histograms for aborts.

Buckets:

- `gt70` → stronger than –70 dBm
- `70_79` → –70 to –79 dBm
- `80_89` → –80 to –89 dBm
- `90_99` → –90 to –99 dBm
- `lt100` → –100 dBm and weaker

### 7.6 `reasons_sum` and `reasons_sum_mismatch`

| Field | Meaning |
|---|---|
| `reasons_sum` | sum of all counters from `dropped_by_reason` |
| `reasons_sum_mismatch` | `1` if the sum does not match `dropped`, otherwise `0` |

This is a pure consistency check.

### 7.7 `hint_code`, `hint_en`, `hint_pl`

This is an automatic diagnostic hint.

Example codes:

- `GOOD`
- `NO_DATA`
- `WEAK_SIGNAL`
- `T1_SYMBOL_ERRORS`
- `T1_BITFLIPS`
- `SX1276_BUSY_ETHER`
- `C1_OVERLOAD_OR_MULTIPATH`

How to read it:

- treat it as a **hint**, not a verdict
- look at the numbers first, then at the hint

### Most important interpretation of `summary`

`summary` tells you how clean the **parser and RX path** are in the current window.

But it does not tell the full truth about actual reception success for a specific meter.

Therefore:

- `summary` = overall condition
- `meter_window` = real success rate of a specific meter

This is especially important on SX1276.

---

## 8. Event `dropped`

Published for a rejected frame.

Example:

```json
{
  "event":"dropped",
  "reason":"decode_failed",
  "stage":"t1_decode3of6",
  "detail":"invalid_3of6_symbol",
  "mode":"T1",
  "rssi":-86,
  "want":56,
  "got":48,
  "raw_got":48,
  "decoded_len":48,
  "final_len":48,
  "dll_crc_removed":0,
  "suffix_ignored":0
}
```

If `diagnostic_publish_raw: true`, you also get:

- `raw`

| Field | Meaning |
|---|---|
| `event` | always `dropped` |
| `reason` | main reason for rejection |
| `stage` | pipeline stage where the frame failed |
| `detail` | more exact technical detail |
| `mode` | `T1` or `C1` |
| `rssi` | RSSI of this attempt |
| `want` | how many bytes the parser wanted |
| `got` | how many bytes the parser effectively received |
| `raw_got` | how many raw bytes arrived from RX path |
| `decoded_len` | length after decoding, e.g. after 3-of-6 |
| `final_len` | final length after processing |
| `dll_crc_removed` | how many DLL CRC blocks were removed |
| `suffix_ignored` | whether suffix was ignored |
| `raw` | raw HEX for manual analysis |

How to read it:

- `reason` tells **what** failed
- `stage` tells **where**
- `detail` tells **how exactly**

---

## 9. Event `truncated`

Almost the same as `dropped`, but it means the frame was cut off / incomplete.

Fields are the same as in `dropped`.

Practically:

- lots of `truncated` usually mean tail-read issues, collisions, FIFO pressure, interference, or weak signal

---

## 10. Event `rx_path`

This is a live event from the receive path.

Example:

```json
{"event":"rx_path","stage":"receive_probe_start","rssi":-94,"detail":"weak_t1_probe_start"}
```

| Field | Meaning |
|---|---|
| `event` | always `rx_path` |
| `stage` | RX-path stage |
| `rssi` | RSSI at the time of the event |
| `detail` | technical detail, if present |

This is mainly useful for driver and RF debugging.

For daily use, it is often worth disabling.

---

## 11. Event `meter_window`

This is the most important **per-meter** event.

Topic:

- `{diagnostic_topic}/meter/{meter_id}/{mode}/window/{trigger}`

Example:

```json
{
  "event":"meter_window",
  "uptime_ms":812000,
  "listen_mode":"T1 only",
  "trigger":"count",
  "id":"00089907",
  "mode":"T1",
  "elapsed_s":312,
  "count_window":10,
  "count_total":42,
  "avg_interval_s":30,
  "win_avg_interval_s":30,
  "win_interval_n":9,
  "last_rssi":-59,
  "win_avg_rssi":-59
}
```

| Field | Meaning |
|---|---|
| `event` | always `meter_window` |
| `uptime_ms` | device uptime at publication time |
| `listen_mode` | configured receiver mode active when this trigger was published |
| `trigger` | `count` or `time` |
| `id` | meter ID |
| `mode` | `T1` or `C1` |
| `elapsed_s` | length of the current window in seconds |
| `count_window` | how many packets arrived in this window |
| `count_total` | how many packets arrived total since boot |
| `avg_interval_s` | average interval over the full meter lifetime |
| `win_avg_interval_s` | average interval only inside this window |
| `win_interval_n` | how many intervals were used for the window average |
| `last_rssi` | RSSI of the last packet |
| `win_avg_rssi` | average RSSI in this window |

### Two triggers

- `count` — after a given number of packets
- `time` — after a time window

Why both?

- `count` shows regularity better when packets come often
- `time` provides a heartbeat even for slower meters

### Why `meter_window` matters so much

Because it shows the **real reception success of a specific meter**.

If a meter should transmit every 30 s but `win_avg_interval_s` is 90 s, you are losing about 2/3 of frames — even if `summary` looks clean.

---

## 12. Event `dev_err_cleared` (SX1262)

Published once after boot if you enable:

- `clear_device_errors_on_boot: true`
- `publish_dev_err_after_clear: true`

Example:

```json
{"event":"dev_err_cleared","before":4,"before_hex":"0004","after":0,"after_hex":"0000"}
```

| Field | Meaning |
|---|---|
| `event` | always `dev_err_cleared` |
| `before` | device error value before clear |
| `before_hex` | same value in HEX |
| `after` | value after clear |
| `after_hex` | same value in HEX |

How to read it:

- `before != 0` does not have to mean a current failure — it may be a latched state from boot
- `after = 0` means the clear succeeded

---

## 13. Practical reading guide

### 13.1 Start with a sane profile

```yaml
listen_mode: t1                # if you mainly have T1
diagnostic_verbose: false
diagnostic_publish_summary: true
diagnostic_publish_drop_events: true
diagnostic_publish_rx_path_events: false
diagnostic_publish_highlight_only: true
diagnostic_publish_raw: false
sx1276_busy_ether_mode: adaptive   # for SX1276
```

### 13.2 Read things in this order

1. `boot`  
   Did the device boot the way you expect?

2. `summary`  
   Does the RX path look clean or dirty?

3. `meter_window`  
   Does the specific meter arrive as often as it should?

4. `dropped` / `truncated`  
   If not, **where** and **why** does it fail?

5. `rx_path`  
   Only when you need deeper debugging.

### 13.3 Typical conclusions

#### `summary` good, `meter_window` bad

Usually means:

- collisions,
- busy ether,
- cost of `both` mode,
- SX1276 limitations.

**Practical proof — measured in the same apartment-block environment, same 900 s window:**

| | SX1262 (Heltec) | SX1276 (Lilygo) |
|---|---|---|
| `summary drop_pct` | 12% | 2% |
| `summary hint` | OK | GOOD |
| `meter_window count` | 28 / 30 | 17 / 30 |
| `meter_window effectiveness` | ~93% | ~57% |
| RSSI | –74 dBm | –48 dBm |

`summary` shows SX1276 as better. `meter_window` shows the opposite.
SX1276 has a stronger signal and still loses more packets — because `adaptive` cuts problematic receptions before decode, which lowers `drop_pct` without improving real capture.

#### High `dll_crc_failed` with good RSSI

Usually means:

- overload,
- multipath,
- local interference,
- not just “weak signal”.

#### High `preamble_read_failed` / `probe_start_aborted`

Usually means:

- garbage in the ether,
- distant or overlapping meters,
- SX1276 should probably stay on `adaptive`.

#### `win_avg_interval_s` much larger than expected

This is the strongest sign that packets are really being lost.

---

## 14. Example debug-oriented configuration

```yaml
wmbus_radio:
  radio_type: SX1276
  listen_mode: t1
  sx1276_busy_ether_mode: adaptive

  highlight_meters:
    - "00089907"

  diagnostic_topic: "wmbus/lilygo/diag"
  diagnostic_summary_interval: 60s
  diagnostic_publish_summary_15min: true
  diagnostic_publish_summary_60min: false
# summary topics: <diagnostic_topic>/summary, /summary_15min, /summary_60min
  diagnostic_verbose: false
  diagnostic_publish_summary: true
  diagnostic_publish_drop_events: true
  diagnostic_publish_rx_path_events: false
  diagnostic_publish_highlight_only: true
  diagnostic_publish_raw: false
```

---

## 15. Short decision rule for `adaptive`

If you use **SX1276** and do not have a strong reason otherwise — **start with `adaptive`**.

Switch to `normal` only when:

- the RF environment is quiet,
- you only have a few meters,
- `meter_window` and `summary` are stable,
- you do not see busy-ether signs.

Treat `aggressive` as a special-purpose tool, not as the forever setting.
