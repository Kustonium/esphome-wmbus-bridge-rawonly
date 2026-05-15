# Support / Wsparcie

This project is maintained in spare time and is not a general support desk.  
Ten projekt jest rozwijany po godzinach i nie jest ogólnym helpdeskiem.

## Before opening an issue / Zanim otworzysz issue

Read / Przeczytaj:

- [`README.md`](README.md)
- [`docs/START_HERE.md`](docs/START_HERE.md) / [`docs/START_HERE_PL.md`](docs/START_HERE_PL.md)
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) / [`docs/TROUBLESHOOTING_PL.md`](docs/TROUBLESHOOTING_PL.md)
- [`docs/DIAGNOSTIC.md`](docs/DIAGNOSTIC.md) / [`docs/DIAGNOSTIC_PL.md`](docs/DIAGNOSTIC_PL.md)
- [`docs/RADIO_OPTIONS_MINIMAL.md`](docs/RADIO_OPTIONS_MINIMAL.md)
- [`docs/CHIP_SELECTION.md`](docs/CHIP_SELECTION.md) / [`docs/CHIP_SELECTION_PL.md`](docs/CHIP_SELECTION_PL.md)
- [`examples/README.md`](examples/README.md) / [`examples/README_PL.md`](examples/README_PL.md)

## Open an issue for / Otwórz issue dla

- reproducible bugs in `wmbus_radio`  
  powtarzalnych błędów w `wmbus_radio`
- regressions between versions  
  regresji między wersjami
- broken examples from this repository  
  zepsutych przykładów z tego repo
- documentation mistakes  
  błędów w dokumentacji

## Do not open an issue for / Nie otwieraj issue dla

- general ESPHome help  
  ogólnej pomocy z ESPHome
- general Home Assistant help  
  ogólnej pomocy z Home Assistant
- MQTT basics, TLS setup or remote broker configuration  
  podstaw MQTT, konfiguracji TLS albo zdalnego brokera
- random YAML from forums  
  przypadkowych YAML-i z forów
- “it does not work” without logs and versions  
  zgłoszeń typu „nie działa” bez logów i wersji

## If you report a bug, include / Jeśli zgłaszasz błąd, dodaj

- board / hardware
- radio type (`SX1262`, `SX1276` or `CC1101`)
- ESPHome version
- project version / release / commit
- relevant YAML
- startup log including the radio sanity report
- `Have data / odebrano dane` lines if RX works locally
- MQTT errors if publishing fails
- diagnostic output if relevant
- for RX problems: `summary` plus at least one `dropped` / `truncated` event if available
- expected behavior
- actual behavior

## Scope reminder / Przypomnienie zakresu

This project is intentionally **RAW-only** and does not aim to replace `wmbusmeters` on the ESP.  
Ten projekt jest celowo **RAW-only** i nie ma zastępować `wmbusmeters` na ESP.

MQTT connection details, including TLS certificates and remote brokers, belong to ESPHome's standard `mqtt:` component. `wmbus_radio` only publishes when the MQTT client is connected and keeps radio reception running when MQTT is unavailable.  
Szczegóły połączenia MQTT, w tym certyfikaty TLS i zdalne brokery, należą do standardowego komponentu `mqtt:` ESPHome. `wmbus_radio` tylko publikuje, gdy klient MQTT jest połączony, i utrzymuje odbiór radiowy, gdy MQTT jest niedostępne.
