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

For boards with an external FEM, also check the `fem_*` pins, and for modules that gate the antenna path, `rf_sw_pin` (see section 13). `dio2_rf_switch` only selects the TX/RX direction and does not replace that gate.

For `SX1276`, normal boards do not need `tcxo_pin`. TCXO variants, for example LILYGO T3 V3.0 TCXO OLED LoRa32, need an explicit TCXO enable pin:

```yaml
tcxo_pin: GPIO12
```

The component does not detect board wiring. Check the schematic or vendor documentation.

## 13. Frames arrive, but few of them - and every RSSI sits in a narrow band

This one is deceptive because nothing looks broken. Frames decode, `dropped` is low, `DIAG hint` reports `GOOD`. There are just a handful of meters instead of dozens.

What settles it is the **RSSI distribution, not the frame count**.

Collect fifteen minutes and compare the strongest reading with the weakest:

```text
-93, -94, -95, -96, -97, -98 dBm      → 5 dB band, everything just above the floor
-58, -64, -71, -76, -80, -87 dBm      → 29 dB spread, healthy path
```

SX1262 sensitivity at 100 kbps is on the order of -105 dBm. A narrow band glued to that floor does **not** mean there are few meters nearby - if that were so, the values would be scattered. It means only what barely clears the threshold is audible, and everything below vanishes without trace. That is the fingerprint of a receiver truncated by sensitivity.

Check in this order:

1. **The RF switch gate.** If the module needs one and nothing drives it, you lose about 30 dB. On XIAO ESP32S3 + Wio-SX1262:

   ```yaml
   rf_sw_pin: GPIO38
   ```

   The boot log states whether it is driven:

   ```text
   RF switch gate / bramka przelacznika RF: driven high (rf_sw_pin) / sterowana
   ```

   `not configured` on a board that needs it is your answer.

2. **Do not drive that pin from YAML with an `on_boot` action.** The construction below validates, compiles, raises no warning and **does not work** - priority 900 lands in the same setup stage as the `gpio output` component itself, so the write happens before the pin becomes an output:

   ```yaml
   # NOT like this:
   esphome:
     on_boot:
       priority: 900
       then:
         - output.turn_on: lora_rf_sw
   ```

   If your config has it, remove it together with the `output:` block - keeping it alongside `rf_sw_pin` makes ESPHome reject the config on a duplicate pin declaration.

3. **FEM pins**, if the board has an external front end (Heltec V4: `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin`).

4. **The antenna** - connector, pigtail, band. Check this after the above, not before.

The control measurement after a fix is `meter_window.win_avg_interval_s` for a known meter: equal to the real transmit interval means no transmissions are being missed (see section 3).

## 14. `task stack overflow` in logs (XIAO and similar boards)

Symptom: a panic / FreeRTOS message like `Task stack overflow` appears in the serial log, typically after enabling heavier diagnostics or after upgrading to a build with more counters.

The `wmbus_radio` receiver runs in its own RTOS task (separate from ESPHome's main loop), so ESPHome's `loop_task_stack_size` does not change it. Some smaller boards — XIAO ESP32-S3 in particular — can run fine on older builds and then overflow this task on a newer build with more diagnostics.

YAML option:

```yaml
wmbus_radio:
  receiver_task_stack_size: 4096
```

Default is `3072` bytes. Allowed range is `2048..16384`. If you see a stack overflow on XIAO or another small board, try `4096`, then `6144`, then `8192` — increase only as far as needed.

## 15. MQTT is down, but radio should still work

MQTT problems are transport problems, not proof of RF failure.

If the broker is unavailable, credentials are wrong, the remote broker is unreachable, or TLS negotiation fails, ESPHome's MQTT client may log errors. `wmbus_radio` should still continue RX and local logging.

Expected behavior:

```text
Have data / odebrano dane (...)
MQTT unavailable / MQTT niedostepny: skip telegram publish ... radio reception continues
```

TLS, certificates, fingerprints and remote broker details belong to ESPHome's standard `mqtt:` section. They are not configured in `wmbus_radio`.

If local `Have data` lines are visible but the backend receives nothing, debug MQTT first. If there are no `Have data` lines, debug radio/board configuration first.

## 16. LR1121 board looks dead, or S1 is worse than expected

Three failures on this board produce silence with no error at all, so check them
before suspecting the driver:

- **Wrong u.FL socket.** The Waveshare HF board has more than one: the ESP32 WiFi
  front end, the LR1121 2.4 GHz port, and the sub-GHz path through the RF switch.
  Only the last one receives wM-Bus.
- **No console output.** There is no USB-serial bridge on the board — the USB-C
  goes straight to the ESP32-S3 native USB pins. `logger:` needs
  `hardware_uart: USB_SERIAL_JTAG`.
- **`tcxo_voltage: 1.8v`.** The chip then reports `HF_XOSC_START` and never
  reaches the receive path. Use `3.0v`; that value is measured on this board.

`HF_XOSC_START` alone in the boot log is **not** a fault. It latches when the chip
enters `STDBY_XOSC`; if the calibration lines afterwards read
`XOSC=0x0020 IMAGE=0x0000 ALL=0x0000`, the radio is fine and the driver says so.

Frames arriving but truncated: raise `payload_length` — NES frames are 245 raw
bytes, so `255` is the working value.

For S1 specifically, remember the ranking is different from T1: `SX1276` is the
radio proven at the S1 noise floor. See [`CHIP_SELECTION.md`](CHIP_SELECTION.md).

## 17. The board reboots every 15 minutes

Symptom: reception works, then stops and starts again on a regular cycle. Uptime
never passes a quarter of an hour, and counters that reset at boot look absurd.

Cause, if the YAML has a bare `api:` and the board publishes over MQTT: ESPHome
defaults to `api.reboot_timeout: 15min`, and that timer restarts the device when
no Native API *client* is connected. A standalone MQTT receiver normally has no
such client, so it restarts forever.

Fix — every example in this repo now ships it:

```yaml
api:
  reboot_timeout: 0s
```

`api:` stays, so Native API and `time: platform: homeassistant` keep working;
only the watchdog is disabled.

How to confirm rather than assume: with the RX metadata topic enabled, read
`boot_id` and `seq` from `wmbus/<topic_name>/rx`. A board that is restarting
shows a new `boot_id` roughly every 900 s; a healthy one keeps one `boot_id`
while `seq` rises across the 15- and 30-minute marks. Measured here on
2026-08-20/21: 51 distinct `boot_id` per board in one night before the change,
one `boot_id` over 14.1 h after it.

`mqtt.reboot_timeout` is a different mechanism that reacts to losing the broker.
Do not change it as part of this fix.
