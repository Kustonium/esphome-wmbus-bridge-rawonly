# `wmbus_tx` — bench transmitter

[Polska wersja](README_PL.md)

A wM-Bus **transmitter** for a LilyGO/SX1276 board. It exists to generate known
frames so a receiver can be measured against something whose exact bit stream
is known in advance. It is a test instrument, not part of the product.

This is the only transmit code in this repository. The receiving component
deliberately has no TX path (see the note in `components/wmbus_radio/
transceiver.h`), and that has not changed: this lives under `tests/`, ships in
no example, and is selected by no receiver configuration.

## Why it is here

It was written for the LR1121 long-packet work and lived only on the Home
Assistant host, outside version control, which meant a bench result depended on
a file nobody could review or diff. Two defects found in it during that work —
a bit-clocking loop that could spin without feeding the watchdog, and a C1 mode
that never emitted the mode-C indicator — are exactly the kind that an
un-versioned file hides.

## What it does

| mode | line coding | bitrate | on-air payload for an `L`-byte frame |
|---|---|---|---|
| `t1` | 3-of-6 | 100 kb/s | `ceil(1.5 x (L + 1 + 2 x blocks))` |
| `c1` | none | 100 kb/s | `2 + (L + 1 + 2 x blocks)` |
| `s1` | Manchester | 32768 b/s | `2 x (L + 1 + 2 x blocks)` |

`frame` is the complete link-layer body in hex, starting with the L-field; the
component appends format-A DLL CRCs itself. The validator enforces
`L + 1 == len(frame)` and a 256-byte ceiling, which is also the longest frame
wM-Bus can express.

Bits are clocked out in software against the SX1276's DCLK in continuous FSK
mode. The radio owns the timing; this code only has to place the next DATA bit
before the next clock edge.

### Mode C carries an indicator in the payload

Mode C prefixes the link layer with `0x54` and then `0xCD` (format A) or `0x3D`
(format B). Those two bytes sit **after** the sync word, in the payload, which
is where a receiver reads them: `Packet::link_mode()` keys on the leading `0x54`
and `l_field()` takes the length from index 2, not 0.

Until 2026-09-23 this component omitted them, so `mode: c1` emitted a bare DLL
frame that no receiver could classify — it saw the L-field where it expected
`0x54`, called the frame T1, and failed 3-of-6 on data that was never 3-of-6
encoded. The mode had evidently never been run end to end.

## `dclk_diagnostics`

Off by default. When on, each DATA change is timestamped and a report is
published to `wmbus/txgen/diag/dclk` after every successful transmission:
`updates`, `span_us`, `max_us`, `long_gaps`, and up to 64 `[stream_bit, dt_us]`
pairs.

**`span_us` is the useful number, not `long_gaps`.** The DCLK is hardware-exact
(`32 MHz / 320 = 100.000 kb/s`), so `updates` intervals must take
`updates x 10 us`; every 10 us beyond that is one extra DCLK cycle clocked out,
i.e. one extra bit in the air. Noise on the figure is about +/-2 bits, since the
first and last timestamp each jitter. `long_gaps` uses a 1.5-bit-period
threshold and therefore also counts lateness the loop makes up without losing a
cycle — one report showed 13 long gaps against 2.8 bits of real excess.

These are execution timestamps, not a hardware measurement of DCLK edges. The
MQTT client and the instrumentation itself both change the load on the loop.

Measured on 2026-09-22/23: extra bits appear at roughly 100-bit spacing, i.e.
1 ms at 100 kb/s, which is the FreeRTOS tick period ESPHome configures
(`CONFIG_FREERTOS_HZ = 1000`). A clock-rate mismatch is ruled out
quantitatively: one extra bit per hundred is a 1% rate error, some 500x any
crystal tolerance. Raising the board's `cpu_frequency` to 240 MHz reduced it
without removing it.

## Building it

`check.yaml` is a compile-only configuration. Its pins are placeholders and its
broker address is in the documentation range — **do not flash it**. It exists so
this component is compiled somewhere; code that no configuration selects is
never built by anyone, which is how both defects above survived.

```bash
esphome compile tests/tools/wmbus_tx/check.yaml
```

## Using it on real hardware

Transmitting on 868 MHz is regulated. `power` and `interval` default low and the
868 MHz band has duty-cycle limits; 869.7-870.0 MHz does not, which is why bench
work uses 869.850. Keep the frame's meter ID clearly synthetic so a capture
cannot be mistaken for a real device.
