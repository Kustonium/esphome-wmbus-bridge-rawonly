# wmbus_radio diagnostics quick reference

[Polska wersja](DIAGNOSTICS_QUICK_REFERENCE_PL.md)

Diagnostics are intentionally reduced to `diagnostic_mode` presets. Normal users should not configure old `diagnostic_publish_*` flags manually.

| Mode | Intended use | Published data |
|---|---|---|
| `off` | production without diagnostics | no diagnostic MQTT |
| `low` | quick global overview | global `summary` only |
| `normal` | recommended diagnostic mode | `summary`, suggestions, `summary_15min`, `highlight_meters` stats |
| `debug` | troubleshooting selected meters | like `normal` + drop/rx_path, limited to `highlight_meters` |
| `dev` | testbench/development | everything, including raw diagnostics, `summary_60min`, all-meter stats |

Recommended minimal YAML:

```yaml
wmbus_radio:
  - radio_type: SX1262
    topic_name: xiao_s3
    diagnostic_mode: normal
    highlight_meters:
      - "03500001"
```

Notes:

- `summary_60min` belongs to `dev`, unless old flags are forced manually.
- `meter_snapshot` is mainly useful with `highlight_meters`.
- `busy_ether_state` is SX1276-only. For SX1262/CC1101/LR1121 treat it as `n/a`.
- Old `diagnostic_publish_*` options remain compatible/advanced, but are not recommended.
