# Wsparcie

[English version](SUPPORT.md)

Ten projekt jest rozwijany po godzinach i nie jest ogólnym helpdeskiem.

## Zanim otworzysz zgłoszenie

Przeczytaj:

- [README](README_PL.md)
- [Zacznij tutaj](docs/START_HERE_PL.md)
- [Rozwiązywanie problemów](docs/TROUBLESHOOTING_PL.md)
- [Diagnostyka](docs/DIAGNOSTIC_PL.md)
- [Opcje radiowe](docs/RADIO_OPTIONS_MINIMAL_PL.md)
- [Wybór układu](docs/CHIP_SELECTION_PL.md)
- [Przykłady](examples/README_PL.md)

## Zgłaszaj

- powtarzalne błędy w `wmbus_radio`,
- regresje między wersjami,
- niedziałające przykłady z tego repozytorium,
- błędy w dokumentacji.

## Nie zgłaszaj tutaj

- pytań o ogólną obsługę ESPHome,
- pytań o ogólną obsługę Home Assistant,
- pytań o podstawy MQTT, konfigurację TLS albo zdalnego brokera,
- problemów z przypadkowymi plikami YAML z forów,
- problemów typu „nie działa” bez logów i wersji.

## Jeśli zgłaszasz błąd, dołącz

- model płytki / sprzęt,
- typ radia (`SX1262`, `SX1276`, `CC1101` albo `LR1121`),
- wersję ESPHome,
- wersję projektu / wydanie / commit,
- odpowiedni fragment YAML,
- log startowy z raportem poprawności konfiguracji radia,
- linie `Have data / odebrano dane`, jeśli odbiór działa lokalnie,
- błędy MQTT, jeśli publikacja nie działa,
- dane diagnostyczne, jeśli dotyczą problemu,
- przy problemach z odbiorem: `summary` oraz co najmniej jedno zdarzenie `dropped` / `truncated`, jeśli jest dostępne,
- oczekiwane zachowanie,
- rzeczywiste zachowanie.

## Przypomnienie zakresu

Ten projekt jest celowo **RAW-only** i nie ma zastępować `wmbusmeters` na ESP.

Szczegóły połączenia MQTT, w tym certyfikaty TLS i zdalne brokery, należą do
standardowego komponentu `mqtt:` ESPHome. `wmbus_radio` publikuje tylko wtedy,
gdy klient MQTT jest połączony, i utrzymuje odbiór radiowy, gdy MQTT jest niedostępne.
