# Support / Wsparcie

This project is maintained in spare time and is not a general support desk.  
Ten projekt jest rozwijany po godzinach i nie jest ogólnym helpdeskiem.

## Before opening an issue / Zanim otworzysz issue

Please read / Najpierw przeczytaj:

- `README.md`
- `README_PL.md`
- `DIAGNOSTIC.md`
- `DIAGNOSTIC_PL.md`
- `TROUBLESHOOTING.md`
- `TROUBLESHOOTING_PL.md`
- `CHIP_SELECTION.md`
- `CHIP_SELECTION_PL.md`
- `docs/RX_PIPELINE.md` / `docs/RX_PIPELINE_PL.md`

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
- MQTT basics  
  podstaw MQTT
- random YAML from forums  
  przypadkowych YAML-i z forów
- "it does not work" without logs and versions  
  zgłoszeń typu „nie działa” bez logów i wersji

## If you report a bug, include / Jeśli zgłaszasz błąd, dodaj

- board / hardware
- radio type (`SX1262` or `SX1276`)
- ESPHome version
- project version / release / commit
- relevant YAML
- logs
- diagnostic output if relevant
- for RX problems: `summary` plus at least one `dropped` / `truncated` event if available
- expected behavior
- actual behavior

## Scope reminder / Przypomnienie zakresu

This project is intentionally **RAW-only** and does not aim to replace `wmbusmeters` on the ESP.  
Ten projekt jest celowo **RAW-only** i nie ma zastępować `wmbusmeters` na ESP.