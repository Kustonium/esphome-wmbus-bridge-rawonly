# CHIP_SELECTION.md

[Polska wersja](CHIP_SELECTION_PL.md)

Practical radio-selection guide for `wmbus_radio`. Four radios are supported:
`CC1101`, `SX1276`, `SX1262` and `LR1121`.

## Short answer

- **House / a few meters / quiet RF / mostly slow T1** → `SX1276` is often enough.
- **Apartment block / many meters / frequent packets / larger packets** → choose `SX1262`.
- **S1 meters** → on a clean signal `LR1121`, `SX1276` and `SX1262` hear S1 within about 1 dB of each other (bench, 2026-11-17). On real meters `SX1276` still catches more, because it tolerates off-spec transmitters; the antenna and its place decide the rest. `CC1101` is about 5 dB weaker. See [S1 is a separate question](#s1-is-a-separate-question).
- **C1 meters** → `SX1262` in `listen_mode: both` now hears them (10 C1 meters on a T-Beam, re-measured 2026-10-01 after the sync-word fix). `SX1276` in `both` heard 14. See [C1 is a separate question too](#c1-is-a-separate-question-too).
- **Mixed T1 + C1 on one device** → no measurable T1 cost on `SX1262`, `LR1121` or `SX1276` since the 2026-09-30 fix; `CC1101` not re-measured.
- **Two dedicated devices** (`T1-only` and `C1-only`) → no longer needed: one device in `both` is enough on `SX1262`, `LR1121` and `SX1276`.
- **`CC1101`** → only if that is the hardware you already own. It is behind an
  explicit safety gate and is not the chip to buy for this project.
- **`LR1121`** → newest. On a shared antenna it heard more meters than the
  `SX1276` (2026-11-11) and it tied for best on clean S1 (2026-11-17), but it is one
  board and the youngest driver. Treat it as a working starting point, not a
  supported default.

## Why the difference exists

`SX1276` has an older receive architecture and a much smaller effective buffer path for this workload. In a busier RF environment it is more likely to miss, cut, or never fully enter frequent packets.

`SX1262` handles time pressure better. In practice that means it wins where packets are:

- frequent,
- longer,
- surrounded by other activity,
- mixed with T1/C1 scheduling overhead.

`CC1101` is a much older and simpler part. It works, but its read path is the
slowest of the four (the FIFO poll deadline is 1800 µs against 1000 µs on
`SX1276`). Since 2026-11-13 it has its own S1 modem profile; on S1 it is about
5 dB less sensitive than the other three.

`LR1121` is the newest silicon. An earlier version of this page said it **came
last**; that did not survive a fair comparison. It rested on RSSI corrected with a
−11 dB offset that also contained a worse antenna position (the `LR1121` had its
antenna on a very short pigtail). On **one antenna through a splitter**
(2026-11-11, outputs swapped between phases) the `LR1121` reads **6 dB lower** than
the `SX1276` on the same frames, received about **twice as many frames** and
**174–190 meters against 121–136**, and caught 86–98% of the weakest frames the
`SX1276` decoded. A T-Beam `SX1262` on the other splitter output heard 0.84 of its
meters (2026-11-12/13). On clean S1 (2026-11-17) it tied with the best `SX1262`.
On a T1 attenuator bench (2026-08-31) two `SX1262` boards still decoded about
4–5 dB further than it, so on T1 its edge on the shared antenna is not raw
sensitivity.

What stays true: it has no error tolerance in its preamble/sync detection (like
the `SX1262`), so a transmitter a few percent off-rate is lost entirely - see
[S1 is a separate question](#s1-is-a-separate-question).

One flat and one board of each type, so this ranks these five samples rather than
the silicon.

## What matters most in practice

The biggest factors are:

1. **packet interval**,
2. **packet size**,
3. **RF density / apartment-block noise**,
4. **single-mode vs `both` scheduling**,
5. **which link mode you actually need** — T1/C1 and S1 do not rank the radios
   the same way,
6. **the antenna and its place** — a 10 cm antenna instead of the 6 cm one that
   came with the board gave a Heltec V4 about **+5 dB** and a XIAO about **+3 dB**
   per meter (2026-10-15, same boards, same spot), more than most chip-to-chip
   differences on this page.

The number of meters alone is not the whole story. A few fast meters can hurt more than many slow ones.

## Practical selection table

| Situation | `CC1101` | `SX1276` | `SX1262` | `LR1121` |
|---|---|---|---|---|
| Quiet environment, few slow meters | works | good enough | also good | good |
| Apartment block with many nearby meters | weak | acceptable only in easier cases | recommended | good, least proven |
| Fast meters around 30–60 s | weak | often weak | recommended | good |
| Larger packets under time pressure | weak | weak | recommended | good |
| `both` on one device | not recommended | **fine** - no measurable T1 cost | **fine** - no measurable T1 cost | fine - no measurable T1 cost |
| S1 meters near the noise floor | about 5 dB weaker | **recommended** - same sensitivity, tolerates off-spec meters | same sensitivity on a clean signal, no tolerance | same sensitivity on a clean signal, no tolerance |
| C1 meters | untested here | **best** in `both` (14 C1 meters) | **good** in `both`, see below | hears the C1 meters within its (shorter) T1 range |
| Need maximum reliability | no | limited | recommended | not yet provable |
| Availability of a second opinion | wide | wide | wide | one board, one house |

## T1-only conclusion from real tests

In the tested apartment-block environment on ESPHome `2026.3.2`:

- `SX1262` consistently outperformed `SX1276` for dense RF and frequent / large packets,
- `SX1276` with `adaptive` was acceptable mainly for slower meters around **~120–150 s** intervals in that test environment,
- below that practical threshold, losses on `SX1276` grew with packet frequency and size.

This threshold is **practical, not absolute**. It depends on the building, signal levels, and RF load.

Since 2026-11-15 `sx1262_rx_bandwidth` defaults to **234 kHz** (was 312 kHz,
inherited and never measured for T1). Measured on 2026-11-12/13 against an
`LR1121` on a splitter: a T-Beam caught **52.5%** of the `LR1121`'s frames at
−110…−105 dBm with 234 kHz against 30.6% with 312 kHz, heard 0.84 of its meters
against 0.76, and caught 77% of its C1 frames against 60%.

## S1 is a separate question

S1 does not rank the radios the way T1 does, so the T1 table above does not carry
over.

**Measured on a clean signal, 2026-11-17.** A transmitter board sent a correct S1
frame on 869.850 MHz (no real meter there) through 2×30 dB attenuators, stepping
its power down 1 dB at a time, 20–30 frames per step. Each board shared one antenna
with an `LR1121` through a splitter, so every result is a difference to the
`LR1121` **in the same run** (the `LR1121` itself moves 1.5–3 dB between runs).
The threshold is the power at which a board receives half the frames.

| board | chip | vs `LR1121` | 50% threshold, ≈ LilyGO scale |
|---|---|---|---|
| Waveshare | LR1121 | **0 dB** (reference) | ≈ −103.5 dBm |
| LilyGO T-Beam | SX1262 | **0 dB** | ≈ −103.5 dBm |
| LilyGO T3-S3 | SX1276 | **≈ −1 dB** | −102.6 dBm |
| Heltec V4 | SX1262 | **≈ −1 dB** | ≈ −102.5 dBm |
| SIH Reader | CC1101 | **≈ −5 dB** | ≈ −98.5 dBm |
| XIAO ESP32-S3 + Wio | SX1262 | **≈ −8 dB** | ≈ −95.5 dBm |

Accuracy about ±1–2 dB. "LilyGO scale" is the LilyGO's own RSSI plus the measured
difference to the `LR1121` - an approximation, not laboratory dBm at the connector.

What it means:

- **`LR1121`, `SX1276` and `SX1262` hear a clean S1 signal equally**, within a
  decibel. In the same hours on real meters one board caught 256 S1
  transmissions and another 58 - that is not sensitivity.
- **On real meters two things decide.** First, tolerance to the transmitter: real
  meters are off in frequency, chip rate or preamble, and the `SX1276` has an error
  tolerance in its preamble detector (`sx1276_preamble_tolerance`, default 10) that
  `SX1262` and `LR1121` do not have - at −6% chip rate the `SX1276` still decoded
  8.4% of the frames, both `SX1262` boards and the `LR1121` none (2026-09-05).
  Second, the antenna and its place: a LilyGO on its own, longest antenna gained
  about 5 dB over the splitter, about 3.5 dB of it the splitter loss.
- **`CC1101` is about 5 dB weaker on S1**, a real radio difference: meters that
  reach the `SX1276` at −96…−101 dBm are below its threshold, which is why on the
  test site it received no real S1 meter while decoding a strong test frame 99%.
  It runs its own S1 modem profile (TI AN067 Radio Link A) since 2026-11-13.
- **The XIAO sample is about 8 dB weaker** than the other two `SX1262` boards -
  that board with its antenna path, not the chip.

**Withdrawn:** "`SX1262` fails at −85 dBm on S1, 3–10 dB behind `SX1276`"
(2026-08). It compared RSSI read by different boards, which are not on one scale:
at its threshold the Heltec read −87 dBm where the `LR1121` read −109 dBm for the
same signal.

Practical rule for S1: **antenna and place first.** Any of `LR1121`, `SX1276`,
`SX1262` hears clean S1 equally; with many weak, older meters prefer `SX1276` for
its tolerance; avoid `CC1101` for weak S1.

## C1 is a separate question too

**Re-measured on 2026-09-30 / 2026-10-01, after the sync-word fix** (one sync word,
0x543D, in `c1` and `both`; see `RELEASE_NOTES.md`), same flat and boards, real
meters, working day 07:00-16:00:

| receiver, `listen_mode: both` | C1 meters heard | notes |
|---|---|---|
| `SX1262` (LilyGO T-Beam) | **10** | 56 C1 frames/h; was 2 with the bug |
| `LR1121` | 1 | the strongest Techem, received in full; the other C1 meters are beyond its range on T1 too |
| `SX1276` (LilyGO) | **14** | 2026-10-01/02, 18.5 h; the strong ones at 8-9 frames/h, the weakest (−94…−98 dBm) now and then |

**The low C1 figures below were mostly the sync-word bug, not the chips.** The
conclusion drawn from them - that the gap to the `SX1276` cannot be closed in
software - is withdrawn. Kept below as the record of what was measured with
the bug.

### Before the fix (2026-09-25/26)

Measured against real Techem C1 meters, all boards in one room, 868.95 MHz:

| receiver | C1 meters heard | notes |
|---|---|---|
| `SX1276` (LilyGO), default `sx1276_preamble_tolerance: 10` | **8–9** | 100 frames in 2.2 h, 487 in one night |
| `SX1276` with `sx1276_preamble_tolerance: 0` | **1** | the same single meter as the chips below |
| `SX1262` (Heltec V4.2, external LNA) | 1 | |
| `SX1262` (LilyGO T-Beam, no LNA, different vendor and antenna) | 2 | the second one: 4 frames in a night |
| `LR1121` | 1 | |

What was ruled out along the way, each by direct measurement: receiver
bandwidth (234, 312 and 467 kHz), transmitter frequency offset (all meters
within ±4 kHz by the `SX1276` AFC), `min_preamble_bits` 16 / 8 / 0, and the
deviation setting (`LR1121` runs C1 at 50 kHz and behaves the same). On the
`SX1262` the weaker C1 meters do not even raise an interrupt - see
`rx_path.irq_start` in [DIAGNOSTIC.md](DIAGNOSTIC.md).

With the bug in place, the `SX1276` advantage on C1 tracked the error tolerance
of its preamble detector: with the tolerance set to 0 it heard exactly what the
`SX1262` and `LR1121` heard. That part was measured and stands, but the
conclusion that the gap is the chip was not: after the fix the T-Beam went from
2 to 10 C1 meters.

This matters less than it sounds for some meters: Techem heat meters send
their plain proprietary telegram on **T1** and a separate AES-encrypted OMS
telegram on C1. The readable values arrive on T1, which the `SX1262` receives
well; the C1 telegram is useless without the key anyway.

Practical rule: **if you have C1 meters, run `SX1262` in `listen_mode: both`.**
`SX1276` in `both` heard the most C1 meters (14 against 10 on the T-Beam) and
remains the better ear for the weakest ones.

## `both` mode conclusion

**Re-measured after the 2026-09-30 sync-word fix: on `SX1262` and `LR1121`,
`both` no longer costs T1.** Working day 07:00-16:00, same board compared with
itself:

| board | `t1` (before) | `both` after the fix |
|---|---|---|
| `SX1262` LilyGO T-Beam | 152 T1 meters, RSSI p1/p5 −98/−96 dBm | **161** T1 meters, p1/p5 **−98/−96** dBm |
| `LR1121` | 55 T1 meters | 51 T1 meters |
| `SX1262` XIAO, `both` with the bug → `both` fixed, same day | 496 T1 frames/h, 29% of the reference board's frames | **805** frames/h, **47%** |

Different days for the first two rows (the `both` day was busier), so a few
meters either way is noise; the RSSI tail being identical is the clearer signal.

Practical takeaway:

- on `SX1262` and `LR1121`, **`both` is fine** - turn it on if you have C1 meters,
- on `SX1276`, **`both` is fine as well**: a LilyGO in `both` heard 143 T1 meters
  over 07:00-11:00 against 131 and 132 on the same hours in `t1`, with the
  control boards flat (T-Beam 141 → 142). The earlier 119 → 108 was measured
  with the bug,
- the earlier figures (118 → 64 on the T-Beam, 62 → 40 on the `LR1121`, one night
  2026-09-25/26) were the sync-word bug plus a neighbouring antenna on the bench,
  not the cost of `both`.

`both` is T1/C1 only on every radio. **S1 never participates in `both`** and must
be selected explicitly with `listen_mode: s1`, which also changes the default
frequency to 868.300 MHz.

## Recommendation about `adaptive`

For `SX1276`, start with the default:

```yaml
sx1276_busy_ether_mode: normal
```

`adaptive` and `aggressive` do not tune the receiver - they **give up on weak
starts** so the radio can keep up in a busy ether. That is a trade, and it is
only worth making once the receiver is genuinely overrunning.

Measured on a dense block, 2026-08-23, all four boards in one spot:

| mode | weakest frame received | meters heard |
|---|---:|---:|
| `adaptive` | −84 dBm (nothing weaker got through at all) | 27 |
| `normal` | **−97 dBm** | **53** |

Meanwhile `fifo_overrun`, `truncated`, `payload_read_failed` and `irq_timeout`
stayed at zero all day: the receiver was never overrunning, so the sensitivity
was being spent on nothing. That is why the default is now `normal`.

Raise it when the overload counters say so, not because the environment feels
busy:

- `fifo_overrun` > 0 or `truncated` > 0, **and** real losses in `drop_pct`,
- then compare per-meter counts before and after - not `drop_pct`, which
  improves simply because the frames it would have counted are no longer
  attempted.

`aggressive` is for deliberate testing, not for daily use.

This option is `SX1276`-only. On `SX1262`, `CC1101` and `LR1121` there is no
busy-ether state machine, and `busy_ether_state` reads as `n/a`.

**Caveat:** one board, one building, one evening. The mechanism is understood
(the abort threshold is clamped near −86 dBm and the mode pushes it to that
clamp) but the size of the effect elsewhere is unknown.

## Known limits you should accept upfront

- Low `drop_pct` does **not** automatically mean better real reception.
- `summary` can look cleaner on `SX1276` while `meter_window` shows worse real results.
- `both` is always a compromise on one radio.
- Software can improve margins, but it does not remove the hardware class difference between these parts.
- **RSSI values are not comparable between boards.** A board with an external
  LNA/FEM reads 13–15 dB higher on identical frames. On one shared antenna
  (2026-11-11/17) the `LR1121` reads about 6 dB below the `SX1276` LilyGO, and the
  Heltec V4 about 15 dB above it. Comparing absolute dBm across two different
  boards measures the front end, not the reception.
- **Frame counts are only comparable when the boards stand in the same place.**
  They do not, in any of the tests quoted here, so every cross-chip number above
  describes a board in a position, not a chip in isolation. How much the position
  weighs was measured on 2026-10-20/21: moving one SX1262 board's antenna 30-50 cm
  along the same wall gave it about 11% more frames from most meters and cost it
  a whole group of meters it had heard every day before - same board, same
  settings, same antenna, a quiet control board unchanged. At 868 MHz reflections
  cancel every ~17 cm (half a wavelength), and every meter has its own pattern of
  such spots. See TROUBLESHOOTING, section 19.
