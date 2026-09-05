# wmbus_radio — szybka ściąga diagnostyki

[English version](DIAGNOSTICS_QUICK_REFERENCE.md)

Diagnostyka jest celowo uproszczona do presetów `diagnostic_mode`. Normalny użytkownik nie powinien ręcznie ustawiać starych flag `diagnostic_publish_*`.

| Tryb | Dla kogo | Co publikuje |
|---|---|---|
| `off` | produkcja bez diagnostyki | brak diagnostyki MQTT |
| `low` | szybki podgląd globalny | tylko globalne `summary` |
| `normal` | zalecany tryb diagnostyczny | `summary`, sugestie, `summary_15min`, statystyki `highlight_meters` |
| `debug` | szukanie problemu z konkretnymi licznikami | jak `normal` + drop/rx_path, filtrowane do `highlight_meters` |
| `dev` | testbench/deweloperka | wszystko, łącznie z raw diag, `summary_60min` i statystykami wszystkich liczników |

Zalecany minimalny YAML:

```yaml
wmbus_radio:
  - radio_type: SX1262
    topic_name: xiao_s3
    diagnostic_mode: normal
    highlight_meters:
      - "03500001"
```

Uwagi:

- `summary_60min` jest tylko dla `dev`, chyba że wymusisz stare flagi ręcznie.
- `meter_snapshot` ma sens głównie z `highlight_meters`.
- `busy_ether_state` dotyczy SX1276. Dla SX1262/CC1101/LR1121 traktuj jako `n/a`.
- Stare opcje `diagnostic_publish_*` zostały jako kompatybilne/zaawansowane, ale nie są zalecane.
