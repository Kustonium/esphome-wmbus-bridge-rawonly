# wmbus_radio diagnostics quick reference / szybka ściąga diagnostyki

## PL

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
      - "03534159"
```

Uwagi:

- `summary_60min` jest tylko dla `dev`, chyba że wymusisz stare flagi ręcznie.
- `meter_snapshot` ma sens głównie z `highlight_meters`.
- `busy_ether_state` dotyczy SX1276. Dla SX1262/CC1101 traktuj jako `n/a`.
- Stare opcje `diagnostic_publish_*` zostały jako kompatybilne/zaawansowane, ale nie są zalecane.

## EN

Diagnostics are intentionally reduced to `diagnostic_mode` presets. Normal users should not configure old `diagnostic_publish_*` flags manually.

| Mode | Intended use | Published data |
|---|---|---|
| `off` | production without diagnostics | no diagnostic MQTT |
| `low` | quick global overview | global `summary` only |
| `normal` | recommended diagnostic mode | `summary`, suggestions, `summary_15min`, `highlight_meters` stats |
| `debug` | troubleshooting selected meters | like `normal` + drop/rx_path, limited to `highlight_meters` |
| `dev` | testbench/development | everything, including raw diagnostics, `summary_60min`, all-meter stats |

Notes:

- `summary_60min` belongs to `dev`, unless old flags are forced manually.
- `meter_snapshot` is mainly useful with `highlight_meters`.
- `busy_ether_state` is SX1276-only. For SX1262/CC1101 treat it as `n/a`.
- Old `diagnostic_publish_*` options remain compatible/advanced, but are not recommended.
