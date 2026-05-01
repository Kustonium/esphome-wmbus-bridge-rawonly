# Diagnostyka

[English version](DIAGNOSTIC.md)

## Topiki MQTT

Przy:

```yaml
esphome:
  name: xiao-s3

wmbus_radio:
  diagnostic_mode: normal
```

komponent generuje:

```text
wmbus/xiao-s3/telegram
wmbus/xiao-s3/diag/summary
wmbus/xiao-s3/diag/summary_15min
wmbus/xiao-s3/diag/meter_snapshot
wmbus/xiao-s3/diag/boot
```

Użyj `topic_name`, jeśli chcesz nadpisać tylko nazwę urządzenia:

```yaml
topic_name: "xiao_s3"
```

Nie wpisuj `wmbus/` w `topic_name`.

Stare `telegram_topic` i `diagnostic_topic` nadal działają, ale są ręcznym override i generują dwujęzyczny warning.

## Tryby diagnostyczne

| Tryb | Publikacja MQTT |
|---|---|
| `off` | brak diagnostyki MQTT |
| `low` | globalne summary + hint |
| `normal` | globalne summary + summary 15-min + snapshot liczników dla `highlight_meters` |
| `debug` | `normal` + eventy drop/RX-path |
| `dev` | pełna diagnostyka developerska, także raw/debug payloady |

Deprecated aliasy:
- `medium` -> `normal`
- `full` -> `dev`
- `raw` -> `dev`

## Zalecana diagnostyka normalna

```yaml
diagnostic_mode: normal
highlight_meters:
  - "00089907"
  - "03534159"
```

To publikuje:

```text
wmbus/<topic_name>/diag/meter_snapshot
```

i śledzi okna statystyk dla ID z `highlight_meters`.

## Filtrowanie eventów

Nowa jasna nazwa:

```yaml
diagnostic_events_highlight_only: true
```

Ogranicza szczegółowe eventy diagnostyczne do liczników z `highlight_meters`.

Stara nazwa:

```yaml
diagnostic_publish_highlight_only: true
```

nadal działa, ale jest myląca i wygeneruje warning. Ta opcja **nie** włącza statystyk liczników.

## Statystyki liczników

Zalecane:

```yaml
diagnostic_mode: normal
highlight_meters:
  - "00089907"
```

Zaawansowane:

```yaml
diagnostic_meter_stats: highlighted
```

albo:

```yaml
diagnostic_meter_stats: all
```

`all` śledzi każde zdekodowane ID licznika i powinno być używane tylko w developmentcie albo kontrolowanych testach.

## `summary`

Główny topic:

```text
wmbus/<topic_name>/diag/summary
```

Ważne pola:
- `total` — kandydaci przetworzeni przez zwalidowaną ścieżkę ramek,
- `ok` — poprawne ramki,
- `dropped` — odrzucone ramki,
- `crc_failed` — błędy DLL CRC,
- `drop_pct` — globalny procent, przydatny, ale nie decydujący,
- `dropped_by_reason`,
- `dropped_by_stage`,
- `rx_path`,
- `hint_code`,
- `busy_ether_state`.

Dla SX1262 i CC1101:

```json
"busy_ether_state": "n/a"
```

SX1276 może raportować `normal`, `aggressive`, `adaptive_active` albo `adaptive_passive`.

## `meter_snapshot`

Główny topic:

```text
wmbus/<topic_name>/diag/meter_snapshot
```

To najlepsza metryka do testów A/B.

Porównuj:
- `count_window`,
- `win_avg_interval_s`,
- `win_interval_n`.

Nie oceniaj zmian RF tylko po globalnym `drop_pct`. Bardziej agresywny tryb może podnieść `drop_pct`, a jednocześnie odzyskać więcej ramek dla ważnych liczników.

## `listen_mode_filter_after_parse`

Domyślnie:

```yaml
listen_mode_filter_after_parse: false
```

Tryb konserwatywny. Zalecany, gdy liczniki są blisko i stabilne.

Eksperymentalnie:

```yaml
listen_mode_filter_after_parse: true
```

Może pomóc przy dalszych licznikach, ścianach albo częściowo traconych ramkach. Zwykle zwiększa:
- `false_start_like`,
- `payload_size_unknown`,
- `t1_decode3of6`.

Oceniaj po `meter_snapshot`, nie po samym summary.

## Stare szczegółowe opcje

Te opcje nadal się kompilują dla kompatybilności, ale są deprecated/advanced:

```yaml
diagnostic_publish_summary
diagnostic_publish_summary_15min
diagnostic_publish_summary_60min
diagnostic_publish_drop_events
diagnostic_publish_rx_path_events
diagnostic_publish_highlight_only
diagnostic_publish_summary_highlight_meters
diagnostic_publish_raw
diagnostic_verbose
```

Najpierw używaj presetów `diagnostic_mode`.
