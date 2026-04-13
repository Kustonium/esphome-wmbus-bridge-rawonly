# ESPHome wM-Bus Bridge (RAW-only)

[English version](README.md)

Stabilny mostek **wireless M-Bus RF → MQTT** dla **SX1262** i **SX1276**.

Ten projekt zostawia ESP tylko jedno zadanie:
- odbiór ramek wireless M-Bus,
- złożenie telegramu,
- publikację RAW HEX do MQTT,
- pozostawienie dekodowania licznika poza mikrokontrolerem.

To repo celowo **nie dekoduje liczników na ESP**.  
Nie dobiera driverów, nie liczy wartości i nie próbuje zastępować `wmbusmeters`.

## Po co ten projekt?

Wiele projektów wM-Bus na ESP próbuje robić wszystko na urządzeniu.

Ten nie.

Celem jest prostsza i stabilniejsza architektura:
- mniejsze obciążenie CPU i RAM po stronie ESP,
- mniej regresji firmware wynikających z logiki liczników na mikrokontrolerze,
- łatwiejsza diagnostyka RF,
- łatwiejsze utrzymanie,
- końcowe dekodowanie zostaje po stronie **Home Assistant / Linux / wmbusmeters**, gdzie jego miejsce.

## Architektura

```text
licznik -> SX1262/SX1276 -> ESPHome wmbus_radio -> MQTT HEX -> wmbusmeters / Home Assistant
```

## Dla kogo to jest

- dla osób, które chcą stabilne radio na ESP,
- dla osób, które wolą prosty RAW pipeline do MQTT,
- dla osób, które chcą dekodowanie i wyższą diagnostykę poza ESP.

## Szybka decyzja

| Scenariusz | Rekomendacja |
|---|---|
| Dom / spokojny eter / kilka liczników / tylko T1 | `SX1276` zwykle wystarczy |
| Blok / dużo liczników / częste pakiety | Preferowany `SX1262` |
| Mieszane T1 + C1 na jednym urządzeniu | Działa, ale ma realny koszt odbioru |
| Maksymalna niezawodność w środowisku T1/C1 | Dwa urządzenia: `T1-only` + `C1-only` |

Więcej szczegółów:

- **[`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md)**
- **[`BENCHMARKS_PL.md`](BENCHMARKS_PL.md)**

## Dodatek do Home Assistant

To repo dobrze współpracuje z dodatkiem
[`Kustonium/homeassistant-wmbus-mqtt-bridge`](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge).

Surowy HEX z MQTT trafia tam do `wmbusmeters` przez `stdin:hex`.

## Szybki start

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s

wmbus_radio:
  radio_type: SX1262   # albo SX1276
  # ... SPI + piny radia ...
  telegram_topic: "wmbus_bridge/my_receiver/telegram"  # ustaw własny topic MQTT
```

`on_frame` używaj tylko wtedy, gdy chcesz dodać efekty uboczne, np. miganie LED, dodatkowe topiki MQTT albo własną logikę dla każdej ramki.

Używaj osobnego topicu MQTT dla każdego odbiornika.

## Bardziej zaawansowane opcje YAML

Poza minimalną konfiguracją komponent obsługuje także:

- osobny rozmiar stosu taska odbiorczego przez `receiver_task_stack_size`
- wbudowaną publikację RAW przez `telegram_topic`
- opcjonalne kierowanie jednego licznika przez `target_meter_id` i `target_topic`
- tryby filtrowania eteru dla SX1276 przez `sx1276_busy_ether_mode: normal | aggressive | adaptive`
- filtrowanie diagnostyki dla wybranych liczników przez `highlight_meters` i `diagnostic_publish_highlight_only`
- opcjonalne wyróżnianie logów przez `highlight_ansi`, `highlight_tag` i `highlight_prefix`
- czyszczenie błędów urządzenia SX1262 przy starcie przez `clear_device_errors_on_boot`
- opcjonalną publikację wyczyszczonych błędów SX1262 przez `publish_dev_err_after_clear`
- strojenie SX1262, takie jak `dio2_rf_switch`, `has_tcxo`, `rx_gain`, `long_gfsk_packets`
- opcjonalną konfigurację pinów FEM dla Heltec V4 przez `fem_ctrl_pin`, `fem_en_pin` i `fem_pa_pin`

Pełna lista pól i eventów jest opisana w [`DIAGNOSTIC_PL.md`](DIAGNOSTIC_PL.md).

## Co repo zawiera

- jeden publiczny komponent ESPHome: `wmbus_radio`
- wewnętrzną implementację dla:
  - obsługi transceiverów `SX1262` i `SX1276`
  - składania pakietów
  - wewnętrznych helperów wireless M-Bus
  - dekodowania `3of6`
  - obsługi DLL CRC
  - diagnostyki i ścieżki RX
- przykłady dla:
  - `SX1262 / Heltec V3`
  - `SX1262 / Heltec V4`
  - `SX1262 / XIAO ESP32 S3`
  - `SX1262 / XIAO nRF52840`
  - `SX1276 / Lilygo T3-S3`
  - `SX1276 / Heltec V2`
- pliki referencyjne do płytek dołączone do przykładów:
  - pinmapa dla `Heltec V4`
  - obraz płytki dla `Lilygo T3-S3`
  - obraz płytki i PDF referencyjny dla `Heltec V2`
- diagnostykę MQTT:
  - `boot`
  - `summary`
  - `dropped`
  - `truncated`
  - `rx_path`
  - `meter_window`
  - `busy_ether_changed` (zmiany stanu adaptive na SX1276)
  - `suggestion` (ograniczane częstotliwościowo wskazówki diagnostyczne)
  - `dev_err_cleared` (SX1262)

## Mapa dokumentacji

- **[`DIAGNOSTIC_PL.md`](DIAGNOSTIC_PL.md)** — pola MQTT, opcje YAML, znaczenie eventów, krótkie i długie okna summary oraz sposób czytania diagnostyki
- **[`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md)** — praktyczny wybór SX1276 vs SX1262
- **[`BENCHMARKS_PL.md`](BENCHMARKS_PL.md)** — wnioski z benchmarków dla `T1-only` i `both`
- **[`TROUBLESHOOTING_PL.md`](TROUBLESHOOTING_PL.md)** — diagnostyka po objawach
- **[`docs/RELEASE_NOTES.md`](docs/RELEASE_NOTES.md)** — opis zmian release’u, w tym adaptive dla SX1276, nowe eventy diagnostyczne MQTT i rozszerzoną diagnostykę runtime
- **[`docs/PERMISSION.md`](docs/PERMISSION.md)** — krótka notatka dokumentująca wyraźną zgodę upstreamu na fork/publikację oraz publikację pod GPL z atrybucją

## Ważne ostrzeżenie diagnostyczne

Nie traktuj `summary` jako synonimu realnej jakości odbioru.

- `summary` pokazuje czystość parsera / decode,
- `meter_window` pokazuje realną skuteczność odbioru konkretnego licznika.

To jest szczególnie ważne dla **SX1276**, gdzie `adaptive` jest realnym algorytmem okienkowym, a nie mglistym auto-trybem. Raz na okno `summary` sprawdza liczniki false-start-like, `drop_pct`, błędy symboli T1 i FIFO overruns; gdy progi wskazują faktycznie zapchane okno, włącza 5-minutowy hold z ostrzejszym filtrowaniem. Wtedy `summary` może wyglądać dobrze, a `meter_window` nadal pokaże realne straty. Zmiany stanu są publikowane jako `diagnostic_topic/busy_ether_changed`, a wskazówki diagnostyczne jako `diagnostic_topic/suggestion`.

## Ważna uwaga o języku logów

Dokumentacja jest rozdzielona na osobne wersje polską i angielską.

Logowanie runtime przyjmuje praktyczną zasadę:

- najważniejsze komunikaty użytkowe `INFO` / `WARN` / `ERROR` mogą być krótkie i dwujęzyczne `EN / PL`,
- niskopoziomowe komunikaty `DEBUG` / `VERBOSE` pozostają po angielsku,
- nazwy opcji YAML, eventów MQTT i pól JSON pozostają po angielsku jako stabilne API techniczne.

Dzięki temu zwykłe logi są czytelniejsze dla polskiego użytkownika, ale niski poziom debugowania nie zamienia się w bałagan.

## Przykłady

### SX1262
- `examples/SX1262/Heltec V3/SX1262_V3.yaml`
- `examples/SX1262/Heltec V4/SX1262_full_example_LED.yaml`
- `examples/SX1262/XIAO ESP32 S3/xiao.yaml`
- `examples/SX1262/XIAO nRF52840/XIAO nRF52840.yaml`

### SX1276
- `examples/SX1276/LilygoT3S3/SX1276_T3S3_full_example.yaml`
- `examples/SX1276/HeltecV2/SX1276_Heltec_V2_full_example.yaml`

### Pliki referencyjne do płytek
- `examples/SX1262/Heltec V4/V4_pinmap.png`
- `examples/SX1276/LilygoT3S3/LilygoT3S3_ver_1_2.png`
- `examples/SX1276/HeltecV2/HeltecV2.png`
- `examples/SX1276/HeltecV2/WIFI_LoRa_32_V2.pdf`

## Jak powstał ten projekt

Projekt powstał w marcu 2026 w ciągu 26 dni — od zera do działającego release’u z diagnostyką, obsługą dwóch transceiverów i pełną dokumentacją.

Zaczął się od praktycznej potrzeby: istniejące rozwiązania nie działały tak, jak było to potrzebne w realnym użyciu. Projekt rozwijał się iteracyjnie na prawdziwym sprzęcie, z naciskiem na stabilność, dobrą diagnostykę oraz pozostawienie dekodowania liczników poza urządzeniem ESP.

W trakcie prac wykorzystywane były narzędzia AI, takie jak Claude i ChatGPT — do szkicowania kodu, refaktoryzacji, analizowania wariantów implementacji i przyspieszania iteracji. Kierunek projektu, wymagania, weryfikacja, testy na sprzęcie, odrzucanie złych pomysłów i decyzje architektoniczne pozostawały po mojej stronie.

To jest opisane wprost, bo tak właśnie ten projekt powstawał: nie przez bezrefleksyjne kopiowanie wygenerowanego kodu, tylko przez użycie AI jako narzędzia programistycznego, z ciągłą weryfikacją i dopasowaniem całości do realnych ograniczeń sprzętu i praktyki.

## Zgłaszanie błędów

Do zgłaszania błędów używaj GitHub Issues i podawaj:

- dokładną wersję ESPHome
- wersję projektu / release / commit
- istotny fragment YAML
- logi
- dane diagnostyczne, jeśli mają znaczenie
## Licencja

**GPL-3.0-or-later** — patrz `LICENSE` i `NOTICE`.