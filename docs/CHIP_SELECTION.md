# CHIP_SELECTION.md

[Polska wersja](CHIP_SELECTION_PL.md)

Practical radio-selection guide for `wmbus_radio`.

## Short answer

- **House / a few meters / quiet RF / mostly slow T1** → `SX1276` is often enough.
- **Apartment block / many meters / frequent packets / larger packets** → choose `SX1262`.
- **Mixed T1 + C1 on one device** → works, but costs reception quality.
- **Best mixed-mode setup** → use two dedicated devices: `T1-only` and `C1-only`.

## Why the difference exists

`SX1276` has an older receive architecture and a much smaller effective buffer path for this workload. In a busier RF environment it is more likely to miss, cut, or never fully enter frequent packets.

`SX1262` handles time pressure better. In practice that means it wins where packets are:

- frequent,
- longer,
- surrounded by other activity,
- mixed with T1/C1 scheduling overhead.

## What matters most in practice

The biggest factors are:

1. **packet interval**,
2. **packet size**,
3. **RF density / apartment-block noise**,
4. **single-mode vs `both` scheduling**.

The number of meters alone is not the whole story. A few fast meters can hurt more than many slow ones.

## Practical selection table

| Situation | `SX1276` | `SX1262` |
|---|---|---|
| Quiet environment, few slow meters | good enough | also good |
| Apartment block with many nearby meters | acceptable only in easier cases | recommended |
| Fast meters around 30–60 s | often weak | recommended |
| Larger packets under time pressure | weak | recommended |
| `both` on one device | not recommended in meaningful T1 traffic | possible, still a compromise |
| Need maximum reliability | limited | recommended |

## T1-only conclusion from real tests

In the tested apartment-block environment on ESPHome `2026.3.2`:

- `SX1262` consistently outperformed `SX1276` for dense RF and frequent / large packets,
- `SX1276` with `adaptive` was acceptable mainly for slower meters around **~120–150 s** intervals in that test environment,
- below that practical threshold, losses on `SX1276` grew with packet frequency and size.

This threshold is **practical, not absolute**. It depends on the building, signal levels, and RF load.

## `both` mode conclusion

`both` is not just “T1 plus some C1”. It adds scheduling overhead even when real C1 traffic is light.

Practical takeaway:

- on `SX1276`, `both` is generally a bad idea when T1 traffic matters,
- on `SX1262`, `both` can make sense, but it still has a measurable cost,
- if you actually care about reliable mixed-mode reception, use **two devices**.

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

## Known limits you should accept upfront

- Low `drop_pct` does **not** automatically mean better real reception.
- `summary` can look cleaner on `SX1276` while `meter_window` shows worse real results.
- `both` is always a compromise on one radio.
- Software can improve margins, but it does not remove the hardware class difference between `SX1276` and `SX1262`.
