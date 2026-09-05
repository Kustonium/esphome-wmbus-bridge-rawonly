# Diagnostyka

[English version](README_DIAGNOSTICS_SNIPPET.md)

Używaj `diagnostic_mode` zamiast ręcznych flag `diagnostic_publish_*`.

```yaml
wmbus_radio:
  - radio_type: SX1262
    topic_name: xiao_s3
    diagnostic_mode: normal
    highlight_meters:
      - "03500001"
```

Tryby:

- `off` — bez diagnostyki MQTT.
- `low` — tylko globalne `summary`.
- `normal` — zalecane: summary, sugestie, 15-min summary, statystyki wyróżnionych liczników.
- `debug` — diagnostyka wskazanych liczników; dodaje drop/rx_path ograniczone do `highlight_meters`.
- `dev` — tryb developerski, bardzo gadatliwy.

Układ tematów przy `topic_name: xiao_s3`:

```text
wmbus/xiao_s3/telegram
wmbus/xiao_s3/diag
wmbus/xiao_s3/diag/summary
wmbus/xiao_s3/diag/summary_15min
wmbus/xiao_s3/diag/meter_snapshot
wmbus/xiao_s3/diag/boot                 # retain=true
wmbus/xiao_s3/diag/config               # retain=true; zrzut efektywnej konfiguracji
wmbus/xiao_s3/diag/suggestion
wmbus/xiao_s3/diag/summary_60min        # tylko dev
wmbus/xiao_s3/diag/busy_ether_changed   # tylko SX1276 adaptive
```


`busy_ether_state` dotyczy tylko SX1276. `sx1276_busy_ether_mode` może przejść w YAML przy innych radiach, ale SX1262/CC1101 to ignorują i pokazują `n/a`.
