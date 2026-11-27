# Atrybucja i licencja

[English version](ATTRIBUTION.md)

To repozytorium jest udostępniane na licencji GPL-3.0-or-later.

Projekt powstał z inspiracji pracami nad komponentem ESPHome wireless M-Bus z
repo `SzczepanLeon/esphome-components` oraz powiązanymi ścieżkami
`wmbusmeters`. Obecny projekt nie jest kombajnem ESPHome dekodującym liczniki
na ESP. Jest mostem RAW-only RF->MQTT: ESP waliduje i przekazuje telegram HEX,
a dekodowanie liczników celowo zostaje po stronie `wmbusmeters` poza ESP.

Część plików źródłowych zachowuje rodowód strukturalny, nazewniczy lub kodowy z
pierwotnego ekosystemu. Pliki są oznaczone nagłówkami SPDX. Nowsze części
projektu dodają architekturę RAW-only, rozbudowaną diagnostykę, walidowaną
publikację MQTT, ścieżki odbioru SX1262/SX1276/LR1121/CC1101 z trybami T1/C1
i S1 oraz przykład przedłużacza ESP-NOW.

## Kontrybutorzy

Kod wniesiony przez osoby spoza projektu. Każda z nich jest autorem commita w
historii i ma podziękowanie w notach wydania.

| Kto | Wkład | Gdzie |
|---|---|---|
| Alexander Streit ([@probutus](https://github.com/probutus)) | `set_forward_meters()`: podmiana listy przekazywanych liczników w trakcie pracy własną lambdą | [PR #40](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/pull/40), wydanie 1.37 |
| [@hackbaellchen](https://github.com/hackbaellchen) | SX1262: przechwytywanie strumienia przycinane do długości ramki w `listen_mode` `c1`/`both`, z testem hosta | [zgłoszenie #45](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/issues/45), wydanie 1.38 |

Jeżeli wykorzystujesz kod z tego repozytorium, zachowaj licencję
GPL-3.0-or-later i informację o autorstwie/pochodzeniu.
