# ESPHome wM-Bus Bridge RAW-only

[English version](README.md)

Most radiowy wireless M-Bus RAW-only dla ESPHome.

ESP odbiera i waliduje telegramy wM-Bus, a następnie publikuje zweryfikowany RAW HEX do MQTT. Dekodowanie liczników zostaje poza ESP, na przykład w Home Assistant / Linux / `wmbusmeters`.

```text
licznik -> SX1262/SX1276 -> ESPHome wmbus_radio -> MQTT HEX -> wmbusmeters / Home Assistant
```

## Zasada projektu

ESP jest mostem radiowym, nie dekoderem liczników.

Nie robi:
- wyboru driverów `wmbusmeters`,
- deszyfrowania AES,
- sensorów z wartościami liczników,
- zastępowania `wmbusmeters`.

Robi:
- odbiór ramek T1/C1,
- walidację i normalizację telegramów,
- publikację poprawnego HEX telegramu do MQTT,
- diagnostykę RF.

## Schemat topiców MQTT

Zalecany schemat:

```text
wmbus/<device>/telegram
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
wmbus/<device>/diag/boot
```

Dodatek bridge w Home Assistant powinien subskrybować:

```text
wmbus/+/telegram
```

W normalnym YAML-u nie składaj ręcznie topiców. Użyj `topic_name` albo pomiń tę opcję, a komponent użyje `esphome.name`.

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

To generuje:

```text
wmbus/xiao_s3/telegram
wmbus/xiao_s3/diag/...
```

Jeżeli `topic_name` nie jest podany, używany jest `esphome.name`. `friendly_name` nie jest używany do topiców, bo może zawierać spacje, wielkie litery albo znaki spoza ASCII.

`topic_name` przyjmuje tylko litery, cyfry, `_` i `-`. Nie wpisuj `wmbus/`, `/`, `+`, `#` ani spacji.

Stare ręczne ustawienia dalej działają:

```yaml
telegram_topic: "..."
diagnostic_topic: "..."
```

ale są traktowane jako legacy/manual override i generują dwujęzyczny warning.

## Szybki start

Minimalny przykład:

```yaml
substitutions:
  devicename: esphome-wmbus-xiao-s3
  friendly_name: "wMBus Bridge XIAO S3"

esphome:
  name: ${devicename}
  friendly_name: ${friendly_name}

external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s

wmbus_radio:
  radio_type: SX1262
  listen_mode: t1

  # Opcjonalne. Jeśli pominiesz, użyty będzie esphome.name.
  # topic_name: "${devicename}"

  diagnostic_mode: normal

  # Opcjonalne. W diagnostic_mode: normal włącza meter_snapshot
  # dla tych liczników.
  highlight_meters:
    - "00089907"
    - "03534159"
    - "03528221"

  # ... tutaj piny SPI/radia ...
```

## Tryby diagnostyki

Używaj presetów, nie zestawu wielu osobnych flag:

| Tryb | Znaczenie |
|---|---|
| `off` | brak diagnostyki MQTT |
| `low` | globalne `summary` + hint |
| `normal` | `summary` + `summary_15min` + `meter_snapshot` dla `highlight_meters` |
| `debug` | `normal` + eventy drop/RX-path |
| `dev` | pełna diagnostyka developerska, także raw/debug payloady |

Stare tryby są nadal akceptowane jako deprecated aliasy:
- `medium` -> `normal`
- `full` -> `dev`
- `raw` -> `dev`

Stare szczegółowe opcje, takie jak `diagnostic_publish_summary_highlight_meters` i `diagnostic_publish_highlight_only`, dalej się kompilują dla kompatybilności, ale są deprecated/advanced. Najpierw używaj presetów.

`diagnostic_publish_highlight_only` było mylącą nazwą. Ta opcja filtruje szczegółowe eventy diagnostyczne do `highlight_meters`; nie włącza statystyk liczników. Jaśniejsza nazwa to:

```yaml
diagnostic_events_highlight_only: true
```

## Statystyki liczników

Do normalnego użycia:

```yaml
diagnostic_mode: normal
highlight_meters:
  - "00089907"
  - "03534159"
```

To publikuje zbiorczy snapshot na:

```text
wmbus/<device>/diag/meter_snapshot
```

Dla zaawansowanych testów:

```yaml
diagnostic_meter_stats: highlighted
```

albo:

```yaml
diagnostic_meter_stats: all
```

`all` stosuj tylko do developmentu albo kontrolowanych testów w gęstym eterze.

## `listen_mode_filter_after_parse`

Domyślnie:

```yaml
listen_mode_filter_after_parse: false
```

To tryb konserwatywny/stabilny. Zalecany, gdy liczniki są blisko i odbiór jest już dobry.

Tryb eksperymentalny:

```yaml
listen_mode_filter_after_parse: true
```

W tym trybie filtr `listen_mode` działa dopiero po parsowaniu/CRC/fallback, czyli po finalnym ustaleniu trybu T1/C1. Może pomóc, gdy liczniki są dalej, za ścianami albo część ramek jest tracona.

Może też zwiększyć:
- `false_start_like`,
- `payload_size_unknown`,
- dropy `t1_decode3of6`.

Porównuj tę opcję po `meter_snapshot` dla ważnych liczników, a nie po samym globalnym `drop_pct`.

## Uwagi radiowe

- SX1262 jest preferowany w gęstym eterze, przy częstych pakietach i długich ramkach T1.
- SX1276 może działać dobrze, szczególnie w T1-only i spokojniejszym eterze.
- W środowisku mieszanym T1/C1 zwykle lepsze są dwa dedykowane odbiorniki niż jeden odbiornik w `both`.

`busy_ether_state` dotyczy tylko SX1276. Dla SX1262 i CC1101 raportowane jest:

```json
"busy_ether_state": "n/a"
```

## CC1101

Obsługa CC1101 jest dostępna w komponencie, ale nadal ma status eksperymentalny. Wymaga jawnego włączenia w YAML oraz poprawnego podłączenia linii GDO0/GDO2.

## Dokumentacja

- [`DIAGNOSTIC_PL.md`](DIAGNOSTIC_PL.md)
- [`TROUBLESHOOTING_PL.md`](TROUBLESHOOTING_PL.md)
- [`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md)
- [`BENCHMARKS_PL.md`](BENCHMARKS_PL.md)
- [`docs/RX_PIPELINE_PL.md`](docs/RX_PIPELINE_PL.md)

## Zasada supportu

Nie ma logów — nie ma supportu.

Przed pytaniem o pomoc podaj:
- YAML bez haseł,
- pełny log startowy,
- 2-5 minut logu pracy,
- model ESP i radia,
- zdjęcie połączeń, jeśli używasz zewnętrznego radia.
