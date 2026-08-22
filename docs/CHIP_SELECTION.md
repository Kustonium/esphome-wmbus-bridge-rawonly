# CHIP_SELECTION.md

[Polska wersja](CHIP_SELECTION_PL.md)

Practical radio-selection guide for `wmbus_radio`. Four radios are supported:
`CC1101`, `SX1276`, `SX1262` and `LR1121`.

## Short answer

- **House / a few meters / quiet RF / mostly slow T1** → `SX1276` is often enough.
- **Apartment block / many meters / frequent packets / larger packets** → choose `SX1262`.
- **S1 meters you can barely hear** → use `SX1276`. See [S1 is a separate question](#s1-is-a-separate-question).
- **Mixed T1 + C1 on one device** → works, but costs reception quality.
- **Best mixed-mode setup** → use two dedicated devices: `T1-only` and `C1-only`.
- **`CC1101`** → only if that is the hardware you already own. It is behind an
  explicit safety gate and is not the chip to buy for this project.
- **`LR1121`** → newest and the strongest receiver measured here so far, but the
  least proven. Treat it as a working starting point, not a supported default.

## Why the difference exists

`SX1276` has an older receive architecture and a much smaller effective buffer path for this workload. In a busier RF environment it is more likely to miss, cut, or never fully enter frequent packets.

`SX1262` handles time pressure better. In practice that means it wins where packets are:

- frequent,
- longer,
- surrounded by other activity,
- mixed with T1/C1 scheduling overhead.

`CC1101` is a much older and simpler part. It works, but its read path is the
slowest of the four (the FIFO poll deadline is 1800 µs against 1000 µs on
`SX1276`), and its S1 support is a raw sniffer rather than a receive path.

`LR1121` is the newest silicon. On the hardware tested here it decoded frames
well below what the other radios reached in the same house, but it has not been
run for weeks and it has never been compared against another radio **in the same
position**, which is the only comparison that would isolate the chip from the
antenna and the wall it stands next to.

## What matters most in practice

The biggest factors are:

1. **packet interval**,
2. **packet size**,
3. **RF density / apartment-block noise**,
4. **single-mode vs `both` scheduling**,
5. **which link mode you actually need** — T1/C1 and S1 do not rank the radios
   the same way.

The number of meters alone is not the whole story. A few fast meters can hurt more than many slow ones.

## Practical selection table

| Situation | `CC1101` | `SX1276` | `SX1262` | `LR1121` |
|---|---|---|---|---|
| Quiet environment, few slow meters | works | good enough | also good | good |
| Apartment block with many nearby meters | weak | acceptable only in easier cases | recommended | good, least proven |
| Fast meters around 30–60 s | weak | often weak | recommended | good |
| Larger packets under time pressure | weak | weak | recommended | good |
| `both` on one device | not recommended | not recommended in meaningful T1 traffic | possible, still a compromise | possible, untested over time |
| S1 meters near the noise floor | raw sniffer only | **recommended** | weaker, see below | promising, single test |
| Need maximum reliability | no | limited | recommended | not yet provable |
| Availability of a second opinion | wide | wide | wide | one board, one house |

## T1-only conclusion from real tests

In the tested apartment-block environment on ESPHome `2026.3.2`:

- `SX1262` consistently outperformed `SX1276` for dense RF and frequent / large packets,
- `SX1276` with `adaptive` was acceptable mainly for slower meters around **~120–150 s** intervals in that test environment,
- below that practical threshold, losses on `SX1276` grew with packet frequency and size.

This threshold is **practical, not absolute**. It depends on the building, signal levels, and RF load.

## S1 is a separate question

S1 does not rank the radios the way T1 does, so the T1 table above does not carry
over. Measured on 2026-08-01 and 2026-08-14:

- `SX1262` decodes S1 down to roughly **−82 dBm** and fails at **−85 dBm**. Three
  independent methods agreed on that threshold.
- `SX1276` decoded the same real transmission, in the same second, at
  **−99/−100 dBm**.
- The gap is therefore about **3–10 dB**, and in the tested building the whole
  local S1 population arrives at or below the `SX1262` threshold.

**`SX1262` is not defective at S1. It is simply less sensitive there.** No AGC or
register tuning recovered the difference; the experiments that appeared to help
did not survive an A–B–A repeat and were removed from the code.

Practical rule: **`SX1276` for S1, `SX1262` for T1.** If you need both, that is an
argument for two devices rather than one compromise.

`CC1101` in `listen_mode: s1` is an experimental raw sniffer on S-mode sync only,
not a receive path — the driver says so at startup. `LR1121` received S1
correctly on the first attempt, but only from a workshop transmitter at −59 dBm,
which proves the path works and says nothing about sensitivity.

## `both` mode conclusion

`both` is not just “T1 plus some C1”. It adds scheduling overhead even when real C1 traffic is light.

Practical takeaway:

- on `SX1276`, `both` is generally a bad idea when T1 traffic matters,
- on `SX1262`, `both` can make sense, but it still has a measurable cost,
- if you actually care about reliable mixed-mode reception, use **two devices**.

`both` is T1/C1 only on every radio. **S1 never participates in `both`** and must
be selected explicitly with `listen_mode: s1`, which also changes the default
frequency to 868.300 MHz.

## Recommendation about `adaptive`

For `SX1276`, start with:

```yaml
sx1276_busy_ether_mode: adaptive
```

Use `normal` only if:

- the RF environment is calm,
- you have only a few meters,
- `meter_window` looks good,
- there are no clear busy-ether symptoms.

Use `aggressive` only deliberately for testing or very rough environments.

This option is `SX1276`-only. On `SX1262`, `CC1101` and `LR1121` there is no
busy-ether state machine, and `busy_ether_state` reads as `n/a`.

## Known limits you should accept upfront

- Low `drop_pct` does **not** automatically mean better real reception.
- `summary` can look cleaner on `SX1276` while `meter_window` shows worse real results.
- `both` is always a compromise on one radio.
- Software can improve margins, but it does not remove the hardware class difference between these parts.
- **RSSI values are not comparable between boards.** A board with an external
  LNA/FEM reads 13–15 dB higher on identical frames. Comparing absolute dBm
  across two different boards measures the front end, not the reception.
- **Frame counts are only comparable when the boards stand in the same place.**
  They do not, in any of the tests quoted here, so every cross-chip number above
  describes a board in a position, not a chip in isolation.
