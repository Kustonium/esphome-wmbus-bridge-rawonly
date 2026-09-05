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
publikację MQTT oraz ścieżki odbioru skupione na SX1262/SX1276.

Jeżeli wykorzystujesz kod z tego repozytorium, zachowaj licencję
GPL-3.0-or-later i informację o autorstwie/pochodzeniu.
