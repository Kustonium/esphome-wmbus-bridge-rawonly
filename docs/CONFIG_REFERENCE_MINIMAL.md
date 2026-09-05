# wmbus_radio minimal configuration reference

[Polska wersja](CONFIG_REFERENCE_MINIMAL_PL.md)

## Core

| Option | Default | Status | Description |
|---|---:|---|---|
| `radio_type` | required | public | `SX1262`, `SX1276`, `CC1101`, `LR1121` |
| `topic_name` | `esphome.name` | public | topic base name: `wmbus/<topic_name>/...`; no `/`, spaces, `+` or `#` |
| `listen_mode` | `both` | public | `t1`, `c1`, `both` = T1/C1 only; `s1` = experimental S1 only |
| `frequency` | mode default | public | optional override; T1/C1/both default to `868.950 MHz`, S1 to `868.300 MHz` |
| `diagnostic_mode` | `off` | public | `off`, `low`, `normal`, `debug`, `dev` |
| `highlight_meters` | empty | public | meter IDs for highlighting and statistics in `normal/debug`; **does not filter MQTT** |
| `forward_meters` | empty | public | whitelist of IDs published on `wmbus/<topic_name>/telegram`; a list or `true` to use `highlight_meters`; empty forwards everything |
| `publish_rssi` | `false` | public | publish each meter's latest frame RSSI on `wmbus/<topic_name>/rssi/<meter_id>`; see below |
| `receiver_task_stack_size` | `3072` | advanced | separate RX task stack size; range `2048..16384` |
| `listen_mode_filter_after_parse` | `false` | experimental | more aggressive filtering after parsing; evaluate per meter, not just by the global drop percentage |
| `use_noise_floor_threshold` | `false` | experimental | derive the weak-start abort threshold from the MEASURED noise floor instead of the average of successful receptions; `noise_floor_dbm` is always measured, this option only uses it |
| `noise_floor_margin_db` | `6` | experimental | required start level above the noise floor in dB (0–30); only applies with `use_noise_floor_threshold: true` |
| `highlight_ansi` | `false` | public | ANSI colours for highlighted meters in the log |
| `highlight_tag` | `wmbus_user` | public | log tag for highlighted meters |
| `highlight_prefix` | `"★ "` | public | prefix of a highlighted meter's log line |
| `allow_untested_framework` | `false` | safety gate | required to build with the `arduino` framework; compilation otherwise stops |
| `mark_as_handled` | `false` | public | option inside `on_frame:`; marks the frame as handled |

## Listen modes and frequency

| Mode | Meaning | Default frequency | Notes |
|---|---|---:|---|
| `t1` | T1 only | `868.950 MHz` | standard mode for many meters |
| `c1` | C1 only | `868.950 MHz` | separate C1 reception |
| `both` | T1/C1 only | `868.950 MHz` | does not include S1 |
| `s1` | S1 only | `868.300 MHz` | experimental diagnostic/compatibility mode |

`frequency:` is an optional override. If omitted, the component selects the default for the mode. An override for S1 testing:

```yaml
wmbus_radio:
  listen_mode: s1
  frequency: 868.36
```

A valid S1 telegram is published on `wmbus/<topic_name>/telegram` just like valid T1/C1 telegrams. This does not mean the ESP decodes meter values; the backend, such as `wmbusmeters`, still does that.

## Radio-specific options

| Option | Radio | Default | Status | Description |
|---|---|---:|---|---|
| `has_tcxo` | `SX1262` | `false` | public | enable on SX1262 boards with a TCXO; omitting it may leave the radio active but receiving no frames |
| `dio2_rf_switch` | `SX1262` | `true` | public | control the RF switch through DIO2 |
| `rx_gain` | `SX1262` | `boosted` | public | `boosted` or `power_saving` |
| `long_gfsk_packets` | `SX1262` | `false` | public | for long T1 frames; disabling it may truncate/drop them |
| `sx1262_rx_bandwidth` | `SX1262` | `312khz` | public | `312khz` (inherited default, unmeasured for T1), `234khz`, `156khz`. Applies to `listen_mode: t1` **and `both`**; ignored by `c1` and `s1`, whose measured 234.3 kHz setting is fixed |
| `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin` | `SX1262` | none | board-specific | external RF front-end pins, e.g. Heltec V4 |
| `rf_sw_pin` | `SX1262` | none | board-specific | module RF-switch gate; required on XIAO ESP32-S3 + Wio-SX1262 (`GPIO38`), otherwise sensitivity is about 30 dB lower |
| `sx1276_busy_ether_mode` | `SX1276` | `normal` | public | `normal`, `aggressive`, `adaptive`; increase only for **measured** overload (`fifo_overrun`/`truncated` > 0); `adaptive` aborts weak starts and costs about 12 dB of sensitivity |
| `tcxo_pin` | `SX1276` | none | board-specific | optional TCXO-enable pin, driven HIGH before SX1276 initialization |
| `busy_pin` | `SX1262`, `LR1121` | required | public | BUSY line; without it the driver cannot distinguish “not ready yet” from a response |
| `rf_switch` | `SX1262` | none | board-specific | force RF-switch state; use only when required by the board documentation |
| `clear_device_errors_on_boot` | `SX1262` | `false` | advanced | clear device errors at startup; **recommended on TCXO boards**, where `XOSC_START_ERR` otherwise stays set and conveys no useful information |
| `publish_dev_err_after_clear` | `SX1262` | `false` | advanced | publish errors read back after clearing; the only way to see them on a node receiving no frames |
| `cc1101_allow_experimental` | `CC1101` | `false` | safety gate | required to start CC1101 |
| `gdo0_pin`, `gdo2_pin` | `CC1101` | required | public | dual IRQ; single-IRQ CC1101 is unsupported |
| `spi_data_rate` | all | `2000000` (2 MHz) | advanced | SPI clock for **this device**, not the whole bus. Lower it before suspecting the chip: a module on jumper wires can lose bits at 2 MHz even with a healthy 3.3 V supply. Registers may read as defaults and the radio may appear misconfigured. Check `reg_write_retries` in `CC1101 debug status`: a value above zero shows failed write-back verification |
| `lr1121_allow_experimental` | `LR1121` | `false` | safety gate | required to start LR1121 |
| `tcxo_voltage` | `LR1121`, `SX1262` | `3.0v` | public | module TCXO voltage; SX1262 DIO3 is a regulated output, so the wrong voltage is a real TCXO risk |
| `tcxo_startup_ticks` | `LR1121` | `3000` | advanced | TCXO startup delay in 32.768 kHz ticks (~91.6 ms) |
| `rx_bandwidth` | `LR1121` | `234300` | advanced | RX bandwidth in Hz |
| `min_preamble_bits` | `SX1262`, `SX1276`, `LR1121` | `16` | advanced | preamble bits required before reception starts. **Maximum 16 for `listen_mode: t1` and `both`**: the T1 preamble is shorter than 24 bits, so `24`/`32` decode no frames (measured: 184 triggers, 0 frames) and fail validation. `8` works but costs about 16% of meters heard. SX1276 has no `32` setting. This is not the transmitted preamble length, a separate `SetPacketParams` field not exposed in YAML |
| `payload_length` | `LR1121` | `255` | advanced | fixed T1 capture length; the host trims the telegram using the decoded L-field |
| `rx_boosted` | `LR1121` | `true` | advanced | +2 dB sensitivity at a cost of about 2 mA |
| `bitrate` | `LR1121` | `100000` | advanced | GFSK bitrate |
| `deviation` | `LR1121` | `50000` | advanced | GFSK deviation |

`tcxo_pin` is SX1276-only. Use `has_tcxo` for SX1262.

`rf_sw_pin` and `dio2_rf_switch` serve different purposes. DIO2 selects the TX/RX direction inside the chip; `rf_sw_pin` opens the module's RF-switch gate, allowing the antenna path to conduct. Boards that require it need both options.

`wmbus_radio` does not guess board wiring. TCXO, RF-switch and FEM options must follow the board schematic or manufacturer documentation.

## MQTT topics

Prefer `topic_name`.

| Topic | Source | Notes |
|---|---|---|
| `wmbus/<topic_name>/telegram` | every valid frame (or only `forward_meters`) | main output for the bridge/wmbusmeters |
| `wmbus/<topic_name>/rx` | the same valid frame as `telegram` | structured reception metadata; default QoS 1, no retain |
| `wmbus/<topic_name>/diag` | drop/rx_path events and a copy of the boot event | diagnostic root, no retain |
| `wmbus/<topic_name>/diag/summary` | every `diagnostic_summary_interval` | global summary |
| `wmbus/<topic_name>/diag/summary_15min` | every 15 minutes | `normal` and above |
| `wmbus/<topic_name>/diag/summary_60min` | every 60 minutes | `dev` only, unless legacy flags override it |
| `wmbus/<topic_name>/diag/meter_snapshot` | meter snapshot | `normal` and above with `highlight_meters`; all meters in `dev` |
| `wmbus/<topic_name>/diag/boot` | once after startup | `retain=true`; also copied to root `diag` without retain |
| `wmbus/<topic_name>/diag/config` | once after startup | `retain=true`; effective configuration snapshot (`{radio, lines[]}`), used by the add-on diagnostic panel |
| `wmbus/<topic_name>/diag/suggestion` | detected RF anomaly | diagnostic suggestions |
| `wmbus/<topic_name>/diag/busy_ether_changed` | busy-ether state transition | SX1276 with `adaptive` |
| `wmbus/<topic_name>/rssi/<meter_id>` | frame with a real RSSI measurement | only with `publish_rssi: true`; `retain=true` |

### Structured RX metadata

Every telegram admitted by `forward_meters` has a companion JSON message on
`wmbus/<topic_name>/rx`. It does not replace the HEX on `telegram` or contain
decoded meter values. The ESP still only receives and validates RF frames;
database storage belongs to the backend.

Schema 1 contains:

- `boot_id` — identifier of the current ESP boot;
- `seq` — an increasing valid-frame sequence number shared by the source;
- `rx_task_wakeup_us` — `esp_timer` uptime sampled when the RX task wakes after
  the IRQ; not the transmission start or the precise `RX_DONE` instant;
- `meter_id`, `mode` (`T1`, `C1` or `S1`) and `rssi_dbm` (`null` if the
  driver provided no real measurement);
- `received_at` — RECEPTION time in ISO-8601 UTC with milliseconds; absent
  until the board clock is set, such as after reboot before SNTP responds;
- `frame_crc32` — IEEE CRC32 of the final normalized frame bytes published as HEX;
- `frame_length` — the number of those bytes.

`/rx` defaults to QoS 1 (`rx_qos`) and uses `retain=false`. `seq` also increases
for valid frames received while MQTT is disconnected, so the next published
message may reveal a gap. A gap indicates missing events along the
ESP→broker→subscriber path, but alone does not identify which segment lost them.
A new `boot_id` starts a new sequence domain.

Legacy/manual overrides:

| Option | Status | Notes |
|---|---|---|
| `telegram_topic` | legacy | manual override; prefer `topic_name` |
| `diagnostic_topic` | legacy | manual override; prefer `topic_name` |

## Forwarding whitelist

`forward_meters` restricts what reaches `wmbus/<topic_name>/telegram`. A common
use is hearing dozens of neighbours' meters but forwarding only your own.

```yaml
wmbus_radio:
  forward_meters:
    - 44332211
    - 77665544
```

If those meters are already in `highlight_meters`, use `true` to reuse the list:

```yaml
wmbus_radio:
  highlight_meters:
    - 44332211
    - 77665544
  forward_meters: true
```

- Empty (default) or `false` preserves the previous behaviour: forward everything.
- `forward_meters: true` with empty `highlight_meters` **does not** silence the
  stream: the filter stays disabled and a startup warning is logged.
- Enter IDs exactly as the log shows them, using the same notation as `highlight_meters`:
  - `id:44332211` → `- 44332211` (BCD meter, decimal notation),
  - `id:417F0666` → `- "0x417F0666"` (non-BCD meter, e.g. Diehl/IZAR).
- **Quote hexadecimal entries.** Otherwise YAML converts `0x417F0666` to
  `1098843750`, which would enter the decimal list and never match. Validation
  detects this and reports an error with guidance instead of silently ignoring it.
- No knowledge of the meter type is needed: a non-BCD A-field always contains
  A–F, whereas a BCD ID never does. Digits alone mean decimal; letters mean raw.
- The `0x` form also works for BCD meters (`"0x00088888"` = `88888`), because
  every meter has a raw representation.
- At startup the log shows parsed IDs and whether they came from
  `highlight_meters`; `dump_config()` also shows `Forward whitelist:`.
- Filtering runs **after** decoding and DLL CRC verification, using parser-validated IDs.
- Diagnostics still count **all** RF traffic: summaries and RSSI statistics are
  updated before publishing. Only the forwarded RAW stream is restricted.
- `target_meter_id` has its own topic and is **not** subject to this whitelist.

## Per-meter RSSI

`publish_rssi` (default `false`) publishes the signal strength of **each meter's
latest frame**, separately for each board:

```yaml
wmbus_radio:
  publish_rssi: true
```

```text
wmbus/<topic_name>/rssi/<meter_id>    payload: -52
```

- The payload is an integer in dBm, without JSON; `retain=true`.
- Only frames with a **real measurement** are published. If the driver did not
  latch RSSI for that frame, nothing is sent: no `0`, `1` or `-127` sentinel
  that a consumer could mistake for a reading.
- The value is measured during reception (SX1276 at the first byte,
  SX1262/LR1121 at the sync word, CC1101 during readout), not a window average
  or noise measurement after reception.
- The `forward_meters` whitelist applies just as it does to telegrams:
  a frame excluded from `telegram` also has no RSSI publication.
- In the **wMBus MQTT Bridge** add-on, each board provides its own
  `signal_strength` entity for a meter, allowing receiver comparisons.

**What RSSI cannot tell you.** It describes only frames that **arrived and
decoded**. A marginal meter with one frame in ten getting through can appear
to have better RSSI than a stable neighbour: only its best attempts enter the
statistics. Use 15/60-minute reception statistics to judge whether a meter
is getting through, rather than RSSI alone.

## Advanced/development-only

| Option | Default | Status | Description |
|---|---:|---|---|
| `target_meter_id` | `""` | advanced | separate forwarding of one meter |
| `target_topic` | `""` | advanced | topic for `target_meter_id` |
| `target_log` | `true` | advanced | log target-meter matches |
| `publish_radio_raw` | `false` | dev-only | raw radio tap on the fixed `wmbus_bridge/raw` topic; different from a normal telegram |
| `diagnostic_publish_suggestion` | from `diagnostic_mode` preset | advanced | publish `suggestion` events, throttled to one per hour per code; explicit `true`/`false` overrides the preset |

## Deprecated diagnostic aliases

`medium` → `normal`

`full` / `raw` → `dev`

Legacy `diagnostic_publish_*` flags remain for compatibility and exceptional tests.
