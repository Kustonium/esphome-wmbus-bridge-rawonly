# Examples

[Polska wersja](README_PL.md)

Public examples are provided only for SX1262 and SX1276 boards.

CC1101 support exists in the component, but it is still experimental and intentionally not documented here as a normal user path.

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
