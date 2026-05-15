# TROUBLESHOOTING.md

[Polska wersja](TROUBLESHOOTING_PL.md)

Symptom-based troubleshooting for `wmbus_radio`.

## Golden rule

Read diagnostics in this order:

1. `boot`
2. `summary`
3. `meter_window`
4. `dropped` / `truncated`
5. `rx_path`
6. `suggestion`
7. `busy_ether_changed`

If you skip `meter_window`, you can fool yourself. If you skip `suggestion` and `busy_ether_changed`, you can also miss what the repo is already telling you about why SX1276 changed behaviour.

## 1. `summary` looks good, but the meter is still missing packets

Most likely causes:

- `SX1276` is dropping bad starts before decode,
- busy RF environment,
- `both` scheduling overhead,
- the meter is fast enough that misses are visible only in per-meter statistics.

What to check:

- `meter_window.win_avg_interval_s`
- `meter_window.count_window`
- `summary.total` vs your expected meter interval
- whether you are using `listen_mode: both`

Practical conclusion:

A clean `summary` does **not** prove good real reception. Trust `meter_window` first.

## 2. `drop_pct` is low, but real results are bad

Most likely on `SX1276` with `adaptive`.

What it usually means:

- losses happen before decode,
- rejected starts never entered `summary.total`,
- the radio is cleaner on paper than in reality.

What to do:

- look at `meter_window`,
- compare with `listen_mode: t1` instead of `both`,
- keep `sx1276_busy_ether_mode: adaptive` unless you have evidence the environment is calm.

## 3. `meter_window.win_avg_interval_s` is much larger than expected

This is one of the strongest signs of real packet loss.

Example:

- meter should transmit every `30 s`,
- `win_avg_interval_s` is around `90 s`.

That means you are receiving only about one third of expected packets.

Most likely causes:

- frequent collisions,
- busy RF,
- `both` overhead,
- `SX1276` limit under time pressure.

## 4. Many `false_start_like`, `probe_start_aborted`, or `preamble_read_failed`

Most likely causes:

- busy ether,
- weak overlapping traffic,
- distant meters,
- apartment-block noise,
- `SX1276` working close to its practical limit.

What to do:

- on `SX1276`, start with `adaptive`,
- avoid `both` unless necessary,
- focus on `meter_window` for highlighted meters,
- compare day vs night.

## 5. High `dll_crc_failed` with decent RSSI

This usually points to:

- overload,
- multipath,
- local interference,
- not simply “weak signal”.

What to check:

- `summary.avg_ok_rssi`
- `summary.avg_drop_rssi`
- `dropped.stage`
- antenna placement and local RF noise sources

## 6. `truncated` is high

This usually means the frame tail is not being read cleanly.

Possible causes:

- collisions near frame end,
- FIFO / RX pressure,
- weak signal tail,
- heavy time pressure in a busy environment.

What to check:

- `truncated` events with `want`, `got`, `raw_got`,
- whether the issue is specific to one large/frequent meter,
- whether the issue worsens during the day.

## 7. `both` works, but T1 got much worse

That is expected in many real environments.

Why:

- `both` adds switching overhead even when actual C1 traffic is low,
- the cost is especially painful on `SX1276`.

What to do:

- first compare with `listen_mode: t1`,
- if you need reliable mixed mode, use two devices,
- on one device, prefer `SX1262` over `SX1276`.

## 8. Which `sx1276_busy_ether_mode` should I use?

Start here:

```yaml
sx1276_busy_ether_mode: adaptive
```

Stay on `adaptive` if:

- you live in an apartment block,
- you see many false starts,
- `meter_window` is worse than `summary` suggests,
- you do not yet know how calm the environment is.

Try `normal` only if:

- you have few meters,
- the RF environment is calm,
- `meter_window` already looks stable.

Treat `aggressive` as a deliberate test setting, not a default.

## 9. I need a sane diagnostic profile

Typical safe profile:

```yaml
listen_mode: t1
diagnostic_mode: normal
highlight_meters:
  - "12345678"

# Optional, only if you need detailed events limited to highlight_meters:
# diagnostic_events_highlight_only: true

# SX1276 only:
sx1276_busy_ether_mode: adaptive
```

## 10. S1 mode receives nothing

First check the expectation: `listen_mode: s1` is a dedicated S1-only RF profile. It is not part of `both`.

Default frequencies:

- `t1`, `c1`, `both` -> `868.950 MHz`
- `s1` -> `868.300 MHz`

If you are testing devices that may use a shifted S-mode frequency, override it explicitly:

```yaml
listen_mode: s1
frequency: 868.36
```

If valid S1 telegrams are received, they are forwarded to MQTT like T1/C1 telegrams. If nothing appears at all, likely causes are:

- the device is not standard passive S1,
- the device uses a proprietary or polling-based system,
- the actual frequency is different,
- the device transmits very rarely,
- the antenna/location is poor.

Do not debug meter drivers or AES keys until the ESP publishes valid telegrams to MQTT.

## 11. The shortest decision path

- use `SX1262` if reliability matters,
- use `SX1276` only when the environment is easier or the traffic is slower,
- do not trust `summary` alone,
- for mixed T1/C1 environments, two dedicated devices beat one `both` setup.

## 12. Radio is active, but there are no `Have data` lines

Do not start with MQTT or the backend.

First check whether the ESP sees any radio frames locally:

```text
Have data / odebrano dane (...)
```

If this line is missing, the problem is still in the RF / board configuration layer.

For `SX1262`, read the boot sanity report. On TCXO-based boards such as Heltec WiFi LoRa 32 V4, missing:

```yaml
has_tcxo: true
```

can still allow the radio to initialize and print `Radio active`, but RX may be completely silent.

Also check board-level options:

```yaml
dio2_rf_switch: true
long_gfsk_packets: true
rx_gain: boosted
```

For boards with an external FEM, also check the `fem_*` pins.

For `SX1276`, normal boards do not need `tcxo_pin`. TCXO variants, for example LILYGO T3 V3.0 TCXO OLED LoRa32, need an explicit TCXO enable pin:

```yaml
tcxo_pin: GPIO12
```

The component does not detect board wiring. Check the schematic or vendor documentation.

## 13. MQTT is down, but radio should still work

MQTT problems are transport problems, not proof of RF failure.

If the broker is unavailable, credentials are wrong, the remote broker is unreachable, or TLS negotiation fails, ESPHome's MQTT client may log errors. `wmbus_radio` should still continue RX and local logging.

Expected behavior:

```text
Have data / odebrano dane (...)
MQTT unavailable / MQTT niedostepny: skip telegram publish ... radio reception continues
```

TLS, certificates, fingerprints and remote broker details belong to ESPHome's standard `mqtt:` section. They are not configured in `wmbus_radio`.

If local `Have data` lines are visible but the backend receives nothing, debug MQTT first. If there are no `Have data` lines, debug radio/board configuration first.
