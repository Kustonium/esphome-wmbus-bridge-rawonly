# BENCHMARKS.md

[Polska wersja](BENCHMARKS_PL.md)

Measured benchmark conclusions for `wmbus_radio`.

## Scope

These notes summarize real-world comparisons between `SX1276` and `SX1262` in the user's apartment-block environment on ESPHome `2026.3.2`.

They are not synthetic lab benchmarks. They are practical field results.

## Test environment

- same physical location for both radios,
- apartment block,
- `T1-only` tests and separate `both` (`T1+C1`) tests,
- environment with many nearby BMT meters plus fast reference meters,
- both night and day observations.

Main traffic profile used in the analysis:

- ~29 BMT devices, `77 B`, around `121 s`,
- `NES 00089907`, `143 B`, around `30 s`,
- `TCH 90830781`, `56 B`, around `34 s`,
- 2 additional BMT water meters, `77 B`, around `121 s`.

## Critical reading rule

Do not compare `summary` and `meter_window` as if they measured the same thing.

- `summary` measures parser / decode cleanliness,
- `meter_window` measures real per-meter reception success.

This is the main diagnostic trap in these tests.

---

## 1. T1-only

## Key practical result

In this apartment-block environment:

- `SX1262` consistently outperformed `SX1276` for frequent / large packets and dense RF,
- `SX1276` with `adaptive` was mainly acceptable for slower meters around **~120–150 s**,
- the remaining gap looked mostly hardware-driven rather than software-driven.

## Representative measured results

| Meter | Size | Interval | SX1276 night | SX1276 day | SX1262 day |
|---|---:|---:|---:|---:|---:|
| `00089907` | `143 B` | `~30 s` | `65%` | `~40%` | `~100%` |
| `90830781` | `56 B` | `~34 s` | `96%` | `~53%` | `~100%` |
| `03534159` | `77 B` | `~121 s` | — | `~88%` | `~100%` |
| `03528221` | `77 B` | `~121 s` | — | `~100%` | `~100%` |

## What the data suggested

### Loss pattern on `SX1276` was structural, not random

For `00089907`, the observed interval pattern showed repeated losses such as:

`30, 60, 30, 90, 60, 30, 89`

That points to short recovery cycles rather than fully random misses.

### Low `drop_pct` on `SX1276` was misleading

`SX1276` often showed low `drop_pct` in `summary` because `adaptive` removed bad starts before decode, so those events never entered `total`.

That did **not** mean the radio was receiving better.

### Noise floor also differed

In identical RF conditions, `false_start_like` counts were roughly:

- `SX1276`: around `~75` at night,
- `SX1262`: around `~34` at night.

That suggests a real architecture / RX-front-end difference, not just a software-tuning difference.

## Practical threshold

The practical threshold of roughly **~120–150 s** was strongly supported **for this environment**.

Above that range, `SX1276` with `adaptive` could be close to `SX1262`.
Below that range, losses rose quickly with packet frequency and packet size.

This is not a universal law. It is a field-proven threshold for this test building and traffic profile.

---

## 2. `both` mode (`T1+C1`)

## Methodological warning

`both` results must **not** be compared directly with `T1-only` as if they were the same test.

Enabling C1 listening changes the reception model for both chips.

A later fix split meter statistics by `(meter_id, link_mode)`, so mixed T1/C1 data for the same meter ID no longer pollutes the statistics.

## Main finding

In `both`, `summary` gave the opposite impression of reality:

| Radio | `ok` | `dropped` | `hint` |
|---|---:|---:|---|
| `SX1276` | `28/28` | `0` | `GOOD` |
| `SX1262` | `20/24` | `4` | `OK` |

On paper `SX1276` looked better.
`meter_window` showed the opposite.

Same lesson again:

- `summary` measured decode cleanliness,
- `meter_window` measured real reception success.

## Representative measured results — day, `both`

| Meter | Size | Interval | SX1276 T1-only | SX1276 both | SX1262 T1-only | SX1262 both |
|---|---:|---:|---:|---:|---:|---:|
| `00089907` | `143 B` | `~30 s` | `~40%` | `~13%` | `~100%` | `~63%` |
| `90830781` T1 | `56 B` | `~35 s` | `~53%` | `~4%` | `~100%` | `~81%` |
| `03534159` | `77 B` | `~121 s` | `~88%` | `~95%`* | `~100%` | `~100%` |
| `03528221` | `77 B` | `~121 s` | `~100%` | `~41%` | `~100%` | `~54%` |

`*` `03534159` was a special case with unusually strong signal around `-52 dBm`.

## Pure switching cost

In the tested windows, real C1 traffic was low or episodic, yet the mere presence of `both` scheduling still reduced T1 success noticeably.

That means the degradation came from switching overhead itself, not only from actual C1 payload handling.

### Observed cost range

| Cost | SX1276 | SX1262 |
|---|---|---|
| Fast meters (`~30–35 s`) | `-60%` to `-96%` | `-19%` to `-37%` |
| Slower meters (`~121 s`) | roughly `-46%` to `-59%`* | roughly `0%` to `-46%` |

`*` excluding the unusually strong `03534159` case.

## Practical conclusion for `both`

- `SX1276 + both` is generally **not recommended** where T1 traffic matters,
- `SX1262 + both` is usable only if the C1 benefit justifies the T1 cost,
- **two devices** (`T1-only` + `C1-only`) are the best solution for mixed environments.

---

## `summary` vs `meter_window` — practical proof

A direct comparison of the same meter (`00089907`) in the same apartment-block environment, both radios on 160 MHz T1-only, same 900 s window:

| | SX1262 (Heltec) | SX1276 (Lilygo) |
|---|---|---|
| `summary drop_pct` | **12%** | **2%** |
| `summary hint` | OK | GOOD |
| `meter_window count` | **28 / 30** | **17 / 30** |
| `meter_window effectiveness` | **~93%** | **~57%** |
| RSSI | –74 dBm | –48 dBm |

At first glance `summary` suggests SX1276 is better. `meter_window` shows the opposite: SX1262 receives far more real packets from the same meter — despite a weaker signal.

The key rule:

- `summary` measures **decode-path cleanliness**,
- `meter_window` measures **real per-meter reception success**.

SX1276 may look cleaner in `summary` because `adaptive` rejects more problematic receptions before they reach the decode stage. That lowers the reported `drop_pct` — but it does not mean better real-world packet capture.

---

## Final benchmark conclusion

### `SX1276`

Acceptable mainly when:

- the environment is easier,
- meters are slower,
- packet sizes are smaller,
- you can stay in dedicated single-mode listening.

### `SX1262`

The better default when:

- the RF environment is dense,
- packets are frequent,
- packets are larger,
- you need the best reliability,
- mixed T1/C1 reception matters.
