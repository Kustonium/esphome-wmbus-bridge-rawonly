## Diagnostics / Diagnostyka

Use `diagnostic_mode` instead of manual `diagnostic_publish_*` flags.

Używaj `diagnostic_mode` zamiast ręcznych flag `diagnostic_publish_*`.

```yaml
wmbus_radio:
  - radio_type: SX1262
    topic_name: xiao_s3
    diagnostic_mode: normal
    highlight_meters:
      - "03534159"
```

Modes / tryby:

- `off` — no diagnostic MQTT / bez diagnostyki MQTT.
- `low` — global `summary` only / tylko globalne `summary`.
- `normal` — recommended: summary, suggestions, 15-min summary, highlighted meter stats / zalecane: summary, sugestie, 15-min summary, statystyki wyróżnionych liczników.
- `debug` — troubleshooting selected meters; adds drop/rx_path events limited to `highlight_meters` / diagnostyka wskazanych liczników; dodaje drop/rx_path ograniczone do `highlight_meters`.
- `dev` — development/testbench; very noisy / tryb developerski, bardzo gadatliwy.

Topic layout with `topic_name: xiao_s3`:

```text
wmbus/xiao_s3/telegram
wmbus/xiao_s3/diag
wmbus/xiao_s3/diag/summary
wmbus/xiao_s3/diag/summary_15min
wmbus/xiao_s3/diag/meter_snapshot
wmbus/xiao_s3/diag/boot                 # retain=true
wmbus/xiao_s3/diag/suggestion
wmbus/xiao_s3/diag/summary_60min        # dev only
wmbus/xiao_s3/diag/busy_ether_changed   # SX1276 adaptive only
```

`busy_ether_state` is SX1276-only. `sx1276_busy_ether_mode` may be accepted in YAML for other radios, but SX1262/CC1101 ignore it and report `n/a`.

`busy_ether_state` dotyczy tylko SX1276. `sx1276_busy_ether_mode` może przejść w YAML przy innych radiach, ale SX1262/CC1101 to ignorują i pokazują `n/a`.
