# Attribution and licensing / Atrybucja i licencja

## EN

This repository is GPL-3.0-or-later.

It was inspired by the ESPHome wireless M-Bus component work from
`SzczepanLeon/esphome-components` and by related `wmbusmeters` code paths.
The current project is not a meter-decoding ESPHome all-in-one component. It is
a RAW-only RF->MQTT bridge: the ESP validates and forwards telegram HEX, while
meter decoding is intentionally left to `wmbusmeters` outside the ESP.

Some source files retain structural, naming, or code-level ancestry from the
original ecosystem. They are marked with SPDX headers. Newer parts of the
project add the RAW-only architecture, extended diagnostics, validated MQTT
publishing, and SX1262/SX1276-focused receive handling.

If you reuse code from this repository, keep the GPL-3.0-or-later license and
preserve attribution.

## PL

To repozytorium jest udostępniane na licencji GPL-3.0-or-later.

Projekt powstał z inspiracji pracami nad komponentem ESPHome wireless M-Bus z
repo `SzczepanLeon/esphome-components` oraz powiązanymi ścieżkami
`wmbusmeters`. Obecny projekt nie jest kombajnem ESPHome dekodującym liczniki
na ESP. Jest mostem RAW-only RF->MQTT: ESP waliduje i przekazuje telegram HEX,
a dekodowanie liczników celowo zostaje po stronie `wmbusmeters` poza ESP.

Część plików źródłowych zachowuje rodowód strukturalny, nazewniczy lub kodowy z
pierwotnego ekosystemu. Pliki są oznaczone nagłówkami SPDX. Nowsze części
projektu dodają architekturę RAW-only, rozbudowaną diagnostykę, walidowaną
publikację MQTT oraz ścieżki odbioru skupione na SX1262/SX1276.

Jeżeli wykorzystujesz kod z tego repozytorium, zachowaj licencję
GPL-3.0-or-later i informację o autorstwie/pochodzeniu.
