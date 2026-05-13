# Examples

[Polska wersja](README_PL.md)

Public examples are provided only for SX1262,SX1276,CC1101 boards.

CC1101 support is available, but still experimental. It requires explicit YAML opt-in and proper GDO0/GDO2 wiring. This example is provided for advanced/testing use, not as the recommended user path.

## Topic model

Examples use the new automatic MQTT topic model.

If `topic_name` is omitted, the component uses `esphome.name` and generates:

```text
wmbus/<esphome.name>/telegram
wmbus/<esphome.name>/diag/...
```

So the Home Assistant bridge add-on should subscribe to:

```text
wmbus/+/telegram
```

Do not copy old `telegram_topic` / `diagnostic_topic` snippets unless you intentionally need a legacy/manual override.


## Listen modes and frequency

The examples start with:

```yaml
listen_mode: t1
```

Available receive modes:

```text
t1    -> T1 only, default 868.950 MHz
c1    -> C1 only, default 868.950 MHz
both  -> T1/C1 only, default 868.950 MHz
s1    -> S1 only, default 868.300 MHz
```

`both` means T1/C1 only. S1 is a separate receive mode and must be selected explicitly with `listen_mode: s1`.

`frequency:` is optional. Use it only when you intentionally need to override the mode default, for example for compatibility testing:

```yaml
wmbus_radio:
  listen_mode: s1
  frequency: 868.36
```

If a valid S1 telegram is received, the component publishes the raw telegram to MQTT the same way as T1/C1. Decoding meter values still depends on the backend, driver and encryption key.

## Diagnostic model

Examples use:

```yaml
diagnostic_mode: normal
```

This gives:
- global `summary`,
- `summary_15min`,
- `meter_snapshot` for IDs listed in `highlight_meters`.

For a quieter setup use:

```yaml
diagnostic_mode: low
```

For deeper troubleshooting use:

```yaml
diagnostic_mode: debug
```

## File naming

Each board has two variants:

- `*_clean.yaml` — minimal practical config,
- `*_commented.yaml` — same idea with comments and explanations.
