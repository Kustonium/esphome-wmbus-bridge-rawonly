# CC1101: osobne testy synchronizacji i restartowania RX

[English](CC1101_RX_BENCH.md)

Opcje testują hipotezę strat mocnych ramek. Nie są potwierdzoną poprawą czułości.
Wszystkie fazy zbudować z tego samego commita dev. Nowe opcje domyślnie nie zmieniają odbioru.

| Opcja `wmbus_radio` | Domyślnie | Działanie |
|---|---|---|
| `cc1101_sync_15_16` | `false` | T1/C1: 0x06 (16/16 + CS) lub 0x05 (15/16 + CS — **oba z bramką carrier sense**, SWRS061I s. 44). S1 ma zawsze 0x05 z noty TI (15/16 + CS), niezależnie od tej opcji i `cc1101_carrier_sense`. Zmierzone 16.11.2026: bez CS szum trafiał synchronizację S1 ~230/min (15/16) albo ~25/min (16/16), wszystko ze złym CRC, i żadna prawdziwa słaba ramka S1 nie przeszła; mocną fałszywą ramkę S1 przy 0x05 zdekodował 714/720. CC1101 nie słyszy słabych liczników S1, które dekodują SX1276/LR1121. |
| `cc1101_carrier_sense` | `true` | `false` = sync bez carrier sense (0x02, z `sync_15_16` 0x01). Tylko w YAML: od 16.11.2026 nie ma tego na kafelku dodatku (wyłączenie zmierzone jako szkodliwe); wartość zapisana przez starszą wersję jest kasowana przy starcie. |
| `cc1101_rx_bandwidth` | `232khz` | Filtr RX T1/C1: `232khz` (domyślne od 16.11.2026, zmierzone jako lepsze), `325khz` (TI AN067 link B), `270khz`. Od 16.11.2026 tylko w YAML; wartość zapisana z dodatku w czasie testu jest kasowana przy starcie. |
| `cc1101_rx_state_wait` | `false` | SIDLE → czekaj na IDLE → SFRX → SRX → czekaj na RX. |
| `cc1101_idle_rx_refresh` | `false` | Sprawdzanie co około 50 ms podczas oczekiwania na IRQ; odświeżenie tylko przy GDO2=0, pustym FIFO i stanie RX. Dla jednego lub kilku liczników z długimi przerwami w ruchu, gdzie wyciąga odbiornik z zawieszenia. W gęstym ruchu (dziesiątki liczników) kosztuje: zmierzone 19–21.11.2026 przy ok. 350 licznikach (godzinowo A-B-B-A), mocne ramki o 2–3 punkty niżej z odświeżaniem, dlatego domyślnie wyłączone. |

Scalić z istniejącym blokiem radia, zachowując piny:

```yaml
cc1101_sync_15_16: false
cc1101_rx_state_wait: false
cc1101_idle_rx_refresh: false
```

| Osobny eksperyment | A1 | B | A2 | Pozostałe ustawienia |
|---|---|---|---|---|
| Synchronizacja | sync=false | sync=true | sync=false | state_wait=false, refresh=false |
| MARCSTATE | state_wait=false | state_wait=true | state_wait=false | sync stały, refresh=false |
| Odświeżanie | refresh=false | refresh=true | refresh=false | sync i state_wait stałe; zalecane state_wait=true |

O terminie i wyborze faz decyduje prowadzący. Nie przełączać kilku opcji naraz.
Zmiana sync zmienia tylko tolerancję synchronizacji; bramkę CS wyłącza `cc1101_carrier_sense: false`.
Porównywać te same godziny i typ dnia, minimum 2 h, wspólną antenę i niezmieniony
LR1121. Deduplikacja `(source, boot_id, seq)`, emisja CRC32 + kubełek 20 s ±1.
T1 analizować osobno od C1/S1. Raportować emisje/liczniki CC/LR, koszyki RSSI LR,
straty mocnych (-90 < RSSI <= 0) ogółem i per licznik; sprawdzić LR/h i restarty.

## Diagnostyka i ograniczenia

- Raport konfiguracji zawiera oba przełączniki. Zrzut `CC1101 RX bench` pokazuje
  `refreshes` (próby odświeżenia), `skipped` i `state_timeouts`, liczone od startu.
  Nie są to liczniki zgubionych ramek.
- Każdy stan ma limit 10 ms; wymagane są dwa kolejne zgodne odczyty MARCSTATE
  w odstępie 20 us. Bez IDLE nie wysyła SFRX/SRX. Brak RX zgłasza błąd.
  Timeouty są zliczane, komunikaty ograniczone.
- Odświeżanie nie jest wykonywane podczas odczytu ramki. Kontrola GDO2/FIFO/stanu
  jest powtarzana, ale pozostaje krótka luka między ostatnim odczytem a SIDLE.
  Nie gwarantuje to zerowych strat. SPI, kalibracja i planista wydłużają 50 ms.
- Dotychczasowy restart ochronny co około 5 s pozostaje. FOC, BSCFG, PQT, AGC
  i konfiguracja pozostałych układów radiowych nie są zmieniane.
- Te dane nie potwierdzą same w sobie „nośnej bez sync”; do tego potrzebna jest
  osobna obserwacja czasowa CS/GDO2/FIFO. PQT_REACHED przy PQT=0 niczego nie
  dowodzi o jakości preambuły. Test hostowy nie zastępuje pomiaru RF.
- Nie zmieniono konfiguracji działających płytek ani nie uruchomiono OTA.

Podstawa: [TI CC1101 SWRS061I](https://www.ti.com/lit/ds/symlink/cc1101.pdf),
sekcja 10.3 oraz opis SFRX/MARCSTATE.
