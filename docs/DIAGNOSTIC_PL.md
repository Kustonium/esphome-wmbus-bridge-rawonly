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
wmbus/xiao-s3/diag/config              # retain=true; efektywna konfiguracja z tego bootu
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

Dla SX1262, CC1101 i LR1121:

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

### Pola RSSI

`last_rssi` to poziom ostatniej ramki, dla której udało się zmierzyć sygnał. `win_avg_rssi` to średnia z okna.

Uśredniane są wyłącznie ramki z rzeczywistym pomiarem. Ramka, dla której radio nie oddało poziomu, jest raportowana jako **-127 dBm** („nie zmierzono") i **nie wchodzi do średnich** — inaczej zaniżałaby statystykę wartością, która nie jest siłą sygnału. Z tego samego powodu `last_rssi` zachowuje poprzedni zmierzony odczyt zamiast przeskakiwać na wartość znacznika.

Konsekwencje przy czytaniu snapshotu:

- `win_avg_rssi: 0` oznacza **brak zmierzonych próbek w oknie**, a nie 0 dBm. Okno może przy tym zawierać ramki — liczniki pakietów rosną niezależnie od pomiaru.
- `last_rssi: 0` na liczniku, który dopiero się pojawił, oznacza to samo: żadna jego ramka nie miała jeszcze pomiaru.
- Wartości `-126` i `-127` traktuj jako brak danych. Heurystyki RF w komponencie również je pomijają.

Rozkład tych wartości między licznikami jest osobnym narzędziem diagnostycznym — patrz TROUBLESHOOTING, sekcja o wąskim paśmie RSSI.

Te pola żyją wewnątrz payloadów diagnostycznych, więc wymagają trybu diagnostycznego. `publish_rssi` to osobna, niezależna opcja: publikuje poziom ostatniej ramki każdego licznika na `wmbus/<topic_name>/rssi/<meter_id>` **niezależnie od `diagnostic_mode`**, jako zwykłą liczbę całkowitą, a dla ramek bez pomiaru po prostu milczy, zamiast wysyłać znacznik. Pól diagnostycznych używaj do czytania obrazu RF płytki; `publish_rssi` — żeby dostać w Home Assistant osobną encję siły sygnału per licznik i per płytka. Pełny opis w CONFIG_REFERENCE_MINIMAL.

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

## Raporty sanity podczas startu

Logi startowe zawierają informacje sanity radia, zanim zacznie się normalne diagnozowanie.

Dla `SX1262` raport sanity pokazuje skuteczne wartości YAML dla:

- `has_tcxo`
- `dio2_rf_switch`
- `long_gfsk_packets`
- `rx_gain`

Ryzykowne ustawienia są wypisywane jako warningi. Nie blokują startu, bo część użytkowników może świadomie testować niepełne albo nietypowe konfiguracje.

Dla SX1262 dostępne są dwie dodatkowe opcje YAML sterujące obsługą błędów urządzenia przy starcie:

- `clear_device_errors_on_boot: true|false` — gdy `true`, komponent wysyła komendę Semtech `ClearDeviceErrors` przy starcie, dzięki czemu zatrzaśnięte błędy z poprzedniego cyklu zasilania nie pozostają aktywne.
- `publish_dev_err_after_clear: true|false` — gdy `true`, snapshot `dev_err` SX1262 (przed/po wyczyszczeniu) jest jednorazowo publikowany po starcie do celów diagnostycznych.

Dla `SX1276` raport sanity pokazuje, czy `tcxo_pin` jest skonfigurowany. Brak `tcxo_pin` jest OK dla zwykłych płytek SX1276. Warianty TCXO, takie jak LILYGO T3 V3.0 TCXO OLED LoRa32, wymagają jawnego pinu zależnego od płytki, np. `tcxo_pin: GPIO12`.

Te raporty opisują konfigurację YAML. Nie wykrywają automatycznie okablowania płytki.

## Dostępność MQTT

Publikacja MQTT jest celowo oddzielona od odbioru radiowego.

Jeżeli MQTT jest niedostępne, odebrane ramki nadal są logowane lokalnie, a publikacja MQTT jest pomijana z ograniczanym czasowo ostrzeżeniem. To pomaga oddzielić problemy RF od problemów transportu.

Jeżeli lokalnie widzisz `Have data / odebrano dane`, ale backend nic nie odbiera, debuguj MQTT. Jeżeli nie widzisz lokalnych linii `Have data`, najpierw debuguj RF i konfigurację płytki.

## Raport konfiguracji przy starcie

Przy każdym starcie, i ponownie przy cyklicznej linii boot-log, komponent
wypisuje **efektywną konfigurację tej płytki** — jedna opcja na linię, oznaczona
tak, żeby dało się odróżnić wybór od dziedziczenia:

```text
[I][wmbus] Configuration / konfiguracja (SX1262):
[I][wmbus]   [core]
[I][wmbus]   radio_type: SX1262 (required)
[I][wmbus]   listen_mode: t1 (CHANGED, default: both)
[I][wmbus]   receiver_task_stack_size: 6144 (CHANGED, default: 3072)
[I][wmbus]   [pins]
[I][wmbus]   cs_pin: GPIO41 (set)
[I][wmbus]   rf_sw_pin: GPIO38 (set)
[I][wmbus]   [sx1262]
[I][wmbus]   has_tcxo: true (CHANGED, default: false)
[I][wmbus]   rx_gain: boosted (default)
```

Znaczniki:

| znacznik | znaczenie |
|---|---|
| `(required)` | wymagane; `radio_type` |
| `(set)` | brak domyślnej w schemacie — pin, nazwa topiku |
| `(default)` | obecne i równe domyślnej ze schematu |
| `(CHANGED, default: X)` | ustawione przez Ciebie i różne od `X` |
| `not set (default: X)` | nieobecne; obowiązuje `X` |

Lista powstaje przy kompilacji **ze schematu**, więc domyślna nie może rozjechać
się z tym, czego naprawdę używa sterownik. Wypisywane są wyłącznie opcje
dotyczące wybranego radia — `has_tcxo` nigdy nie pojawi się przy `CC1101`.

Po co to jest: wcześniej log pokazywał kilka ręcznie wybranych kontroli, więc
wszystko poza tą listą było niewidoczne, a źle skonfigurowana płytka i tak
wyglądała zdrowo. Z raportem pytanie od użytkownika da się rozstrzygnąć z samego
logu, a zdanie „przecież tego nie zmieniałem" staje się sprawdzalne.

## Kontrole sanity per radio

Poza raportem każde radio loguje te kontrole, których niepowodzenie jest
**ciche** — płytka startuje, log wygląda dobrze, a odbioru nie ma albo jest zły:

- **SX1262** — `has_tcxo` (płytka z TCXO bez tego może nie odbierać nic),
  `dio2_rf_switch`, `long_gfsk_packets` (długie ramki T1), `rx_gain` oraz
  **`rf_sw_pin`**: wymagany na XIAO ESP32-S3 + Wio-SX1262 (`GPIO38`), a bez niego
  płytka jest głucha o jakieś 30 dB, wyglądając przy tym całkiem zdrowo.
- **SX1276** — `tcxo_pin` (LilyGO T3 V3.0 TCXO używa `GPIO12`).
- **CC1101** — bramka eksperymentalna oraz `gdo0_pin`/`gdo2_pin`, żeby okablowanie
  dwóch przerwań było potwierdzone, a nie domniemane.
- **LR1121** — `tcxo_voltage`, `tcxo_startup_ticks`, `rx_bandwidth` wobec
  wymaganego `2*fdev + bitrate`, `payload_length`, `rx_boosted` oraz etapy
  kalibracji przy znanym stanie przejściowym `HF_XOSC_START`.
