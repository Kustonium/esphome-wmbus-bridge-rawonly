# START_HERE_PL.md

[English version](START_HERE.md)

Ten plik jest ścieżką czytania tego repozytorium.

W repo jest dużo plików Markdown, bo każdy odpowiada na inne pytanie. Jeśli zaczynasz od zera, nie czytaj wszystkiego naraz. Idź po kolei.

## 0. Najpierw zrozum, czym jest ten projekt

To **nie jest** all-in-one dekoder liczników w ESPHome.

Urządzenie ESP jest tylko odbiornikiem RF Wireless M-Bus i publisherem MQTT.

```text
licznik -> SX1262/SX1276/CC1101 -> ESPHome wmbus_radio -> MQTT -> backend/wmbusmeters -> Home Assistant
```

ESP nie robi:

- wyboru driverów liczników,
- deszyfrowania AES,
- sensorów z wartościami liczników,
- zastępowania `wmbusmeters`.

ESP robi:

- odbiór ramek T1/C1 oraz eksperymentalny odbiór S1,
- walidację i normalizację telegramów,
- publikację poprawnego HEX telegramu do MQTT,
- diagnostykę RF.

Jeśli chcesz jeden YAML ESPHome, który od razu tworzy sensory Home Assistant bez backendu, to nie jest najkrótsza ścieżka.

## 1. Wybierz sprzęt

Przeczytaj:

- [`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md)
- [`examples/README_PL.md`](../examples/README_PL.md)

Zalecana kolejność dla początkujących:

1. **XIAO ESP32-S3 + Wio-SX1262** — kompaktowy, bez OLED-a, bardzo dobry praktyczny target.
2. **Heltec WiFi LoRa 32 V4** — dobra płytka dev z SX1262, ale V4 wymaga poprawnego YAML-a z FEM/RF switch.
3. **Heltec WiFi LoRa 32 V2 / LilyGo T3-S3 SX1276** — używalne targety SX1276.
4. **CC1101** — eksperymentalnie, tylko zaawansowane/testowe użycie. Wymaga `GDO0 + GDO2` i jawnego włączenia w YAML.

Ważne: nie kopiuj YAML-i między Heltec V2/V3/V4 w ciemno. V4 nie jest zamiennikiem drop-in dla starszych płytek.

## 2. Wybierz pasujący przykład YAML

Wejdź do [`examples/`](../examples/).

Użyj pliku `*_clean.yaml`, jeśli rozumiesz już płytkę.

Użyj pliku `*_commented.yaml`, jeśli chcesz wiedzieć, po co są konkretne piny/opcje.

Aktualne grupy przykładów:

```text
examples/SX1262/Heltec V4/
examples/SX1262/Heltec V3/
examples/SX1262/XIAO ESP32 S3/
examples/SX1276/HeltecV2/
examples/SX1276/LilygoT3S3/
examples/CC1101/
```

Do normalnego użycia zostaw:

```yaml
diagnostic_mode: normal
```

Dla znanych liczników dodaj:

```yaml
highlight_meters:
  - "12345678"
```

To włącza snapshoty per licznik dla najważniejszych liczników.


## 3. Wybierz tryb odbioru i częstotliwość

Dla większości instalacji zacznij od T1 albo T1/C1:

```yaml
wmbus_radio:
  listen_mode: t1
```

```yaml
wmbus_radio:
  listen_mode: both
```

`both` oznacza tylko T1/C1. Nie obejmuje S1.

S1 jest eksperymentalnym, osobnym trybem odbioru:

```yaml
wmbus_radio:
  listen_mode: s1
```

Domyślne częstotliwości zależą od trybu:

- `t1`, `c1`, `both` -> `868.950 MHz`
- `s1` -> `868.300 MHz`

Używaj `frequency:` tylko wtedy, gdy musisz nadpisać domyślną wartość, na przykład do testów kompatybilności S1:

```yaml
wmbus_radio:
  listen_mode: s1
  frequency: 868.36
```

Jeżeli poprawny telegram S1 zostanie odebrany, zostanie przekazany do MQTT tak jak inne zweryfikowane telegramy wM-Bus. Dekodowanie po stronie backendu nadal zależy od typu urządzenia, drivera i klucza szyfrowania.

## 4. Topiki MQTT: nie składaj ich ręcznie

Przeczytaj:

- [`docs/CONFIG_REFERENCE_MINIMAL.md`](CONFIG_REFERENCE_MINIMAL.md)

Zalecana konfiguracja:

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

To tworzy topiki:

```text
wmbus/xiao_s3/telegram
wmbus/xiao_s3/diag/summary
wmbus/xiao_s3/diag/summary_15min
wmbus/xiao_s3/diag/meter_snapshot
wmbus/xiao_s3/diag/boot
```

Jeśli `topic_name` nie jest podany, używany jest `esphome.name`.

Stare ręczne ustawienia typu `telegram_topic` i `diagnostic_topic` nadal działają, ale nie powinny być używane w nowych konfiguracjach, chyba że naprawdę tego potrzebujesz.

## 5. Wgraj ESP i najpierw sprawdź tylko warstwę RF/MQTT

Zanim skonfigurujesz liczniki w backendzie, udowodnij, że odbiornik ESP działa.

Sprawdź, czy MQTT dostaje telegramy na:

```text
wmbus/<device>/telegram
```

Sprawdź diagnostykę:

```text
wmbus/<device>/diag/boot
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
```

Nie debuguj kluczy AES, driverów liczników ani encji Home Assistant, dopóki nie wiesz, że RF/MQTT działa.

## 6. Zrozum diagnostykę

Przeczytaj:

- [`DIAGNOSTIC_PL.md`](DIAGNOSTIC_PL.md)
- [`docs/DIAGNOSTICS_QUICK_REFERENCE.md`](DIAGNOSTICS_QUICK_REFERENCE.md)

Dla normalnego użytkownika:

```yaml
diagnostic_mode: normal
```

To daje:

- globalne summary,
- summary 15-minutowe,
- `meter_snapshot` dla `highlight_meters`,
- sugestie RF, jeśli preset je włącza.

`debug` albo `dev` używaj tylko do krótkich testów. Nie trzymaj pełnej diagnostyki developerskiej cały czas.

## 7. Jeśli coś nie działa, idź ścieżką troubleshooting

Przeczytaj:

- [`TROUBLESHOOTING_PL.md`](TROUBLESHOOTING_PL.md)

Krótka wersja:

1. Czy cokolwiek publikuje się na `wmbus/<device>/telegram`?
2. Co mówi `diag/boot`?
3. Co mówi `diag/summary`?
4. Czy ważne liczniki widać w `meter_snapshot`?
5. Czy dropy są losowe, CRC, false-start-like czy związane z trybem?
6. Dopiero po potwierdzeniu RF/MQTT debuguj backend.

## 8. Skonfiguruj backend

To repozytorium jest tylko odbiornikiem RF ESP.

Do dekodowania i Home Assistant MQTT Discovery użyj bridge backendowego:

- <https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge>

Backend powinien subskrybować:

```text
wmbus/+/telegram
```

ID liczników, drivery, klucze AES, JSON i Home Assistant Discovery należą do backendu, nie do komponentu ESP.

## 9. Czytaj głębiej tylko wtedy, gdy trzeba

Używaj tych plików zależnie od pytania:

| Pytanie | Plik |
|---|---|
| Jaką płytkę/radio wybrać? | [`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md) |
| Jak działa diagnostyka? | [`DIAGNOSTIC_PL.md`](DIAGNOSTIC_PL.md) |
| Co znaczą dropy/CRC/interwały? | [`TROUBLESHOOTING_PL.md`](TROUBLESHOOTING_PL.md) |
| Jak wewnętrznie działa RX pipeline? | [`docs/RX_PIPELINE_PL.md`](RX_PIPELINE_PL.md) |
| Jakie są minimalne opcje YAML? | [`docs/CONFIG_REFERENCE_MINIMAL.md`](CONFIG_REFERENCE_MINIMAL.md) |
| Jakie są opcje radiowe? | [`docs/RADIO_OPTIONS_MINIMAL.md`](RADIO_OPTIONS_MINIMAL.md) |
| Co zmieniło się w release? | [`docs/RELEASE_NOTES.md`](RELEASE_NOTES.md) |
| Jak SX1262 wypada względem SX1276? | [`BENCHMARKS_PL.md`](BENCHMARKS_PL.md) |
| Jaki jest zakres projektu/supportu? | [`SUPPORT.md`](../SUPPORT.md) |

## 10. Gdzie pytać

Używaj:

- **Issues** dla odtwarzalnych błędów w komponencie ESP.
- **Discussions** dla pytań, testów sprzętu, działających YAML-i, wyników RF, raportów korekty częstotliwości.
- Issues w repo backendu dla problemów z dekodowaniem, driverami liczników, kluczami AES, JSON albo Home Assistant Discovery.

Przed pytaniem o pomoc podaj:

- model płytki,
- typ radia,
- YAML bez sekretów,
- wersję ESPHome,
- pełny log startowy,
- 2-5 minut logu działania,
- nazwy topików MQTT,
- `diag/boot`, `summary` i `meter_snapshot`, jeśli są dostępne.

Nie ma logów — nie ma supportu.

## 11. Najkrótsza ścieżka

```text
1. Przeczytaj START_HERE_PL.md.
2. Wybierz płytkę w CHIP_SELECTION_PL.md.
3. Skopiuj pasujący YAML z examples/.
4. Użyj diagnostic_mode: normal.
5. Potwierdź telegramy w MQTT.
6. Potwierdź diagnostykę.
7. Skonfiguruj backend bridge.
8. Dopiero potem debuguj drivery/AES/encje Home Assistant.
```
