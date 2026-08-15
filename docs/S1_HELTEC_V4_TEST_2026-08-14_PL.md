# Test S1: Heltec V4 / SX1262 — 2026-08-14

## Cel

Sprawdzić odbiór wM-Bus S1 przez Heltec WiFi LoRa 32 **V4** (nie V4-R8) z SX1262 i zewnętrznym front-endem GC1109 oraz wyjaśnić, dlaczego SX1276 odbierał słabe prawdziwe ramki S1, których Heltec nie widział.

## Konfiguracja odbiornika

- ESP32-S3 + SX1262, ESPHome `2026.7.4`.
- `listen_mode: s1`, `868.300 MHz`, bitrate `32768 bit/s`, FDev `50 kHz`, RxBW `234 kHz`, Manchester/S-mode.
- `has_tcxo: true`, `dio2_rf_switch: true`.
- Heltec V4 FEM: `fem_en_pin: GPIO2`, `fem_ctrl_pin: GPIO7`, `fem_pa_pin: GPIO46`.
- `rx_gain: boosted` (rejestr `0x08AC = 0x96`).
- Stabilny sync sprzętowy: 24 bity `54 76 96`.
- Stos odbiornika: `6144 B`; wcześniejsze `3072 B` było niewystarczające i powodowało stack overflow.

Fabryczny snapshot AGC dla 868 MHz:

```text
CalH=0x01 CalL=0x53
Tune12..Tune13=0x00
FirstPow=0x0A
```

## Co potwierdzono

1. Tor SX1262 i dekoder S1 działają przy stabilnym sygnale. Generator SX1276 z dokręconą anteną dał 9 poprawnych odbiorów na 10 nadań, bez ramek odrzuconych przez CRC. RSSI było stabilne, około `-54..-56 dBm`, a poprawna długość surowej ramki wynosiła `194 B`.
2. Korekcja Manchester per blok CRC odzyskuje ramki już wykryte przez radio. W testach odzyskiwano m.in. 20–29 niepoprawnych par w całej ramce, o ile najgorszy pojedynczy blok mieścił się w limicie 8 erasure.
3. Heltec odbierał generator bez anteny nawet przez ścianę, ale wyniki były skrajnie niestabilne. Nie był to wiarygodny test czułości.
4. W równoczesnym teście prawdziwego eteru SX1276 zdekodował ramkę Lansen `id:00106118` około `-99 dBm`, gdy Heltec nie zgłosił nawet poprawnego sync. Jest to realna obserwacja wymagająca dalszego, długiego porównania z normalnymi antenami.
5. XIAO ESP32-S3 + Wio-SX1262 wymaga `rf_sw_pin: GPIO38`. Mimo poprawnego sterowania w badanym miejscu pozostawał głuchy, podczas gdy Heltec z GC1109 odbierał generator. Zewnętrzny FEM Helteca pomaga; nie potwierdzono, aby był źródłem całkowitej głuchoty.

## Testy, które nie dały poprawy

### `rx_gain`

- `boosted` był wyraźnie lepszy od `power_saving`.
- `power_saving` osiągał około 30% poprawnych odbiorów względem nadań w porównywalnej słabej serii generatora.
- Stan końcowy: pozostawić `boosted`.

### Globalna kalibracja RSSI/AGC (`CalH/CalL`)

Testowano kompensację zewnętrznego LNA o `+17 dB` i `-17 dB`:

- `+17 dB`: 0 poprawnych ramek w obserwowanej serii;
- `-17 dB`: 1 poprawna na 10 przechwyconych i około 14 nadanych;
- obie wartości pogorszyły odbiór względem wcześniejszych okresów fabrycznych.

Eksperymentalna opcja YAML i kod zapisujący te rejestry zostały usunięte.

### `FirstPow` (`0x08B9`)

Testowano fabryczne `0x0A` oraz `0x08` i `0x0C`:

- `0x08`: w krótkiej serii około 25% poprawnych;
- pierwszy długi okres `0x0C`: 37 poprawnych, 55 odrzuconych i 12 pominiętych na 104 nadania (35,6% end-to-end);
- kolejny okres `0x0A`: 0 poprawnych na 15 nadań;
- powrót do `0x0C`: 0 poprawnych na 8 nadań.

Poprawa `0x0C` nie powtórzyła się w układzie A–B–A. Wyniki były zdominowane przez niestabilny tor generatora pracującego bez anteny, dlatego nie można przypisać poprawy rejestrowi. Eksperymentalna opcja YAML i zapis `FirstPow` zostały usunięte.

### Sync S1

Próba skrócenia sprzętowego sync do natywnych 18 bitów zwiększyła liczbę fałszywych wyzwoleń od szumu. Przywrócono odporny wariant 24-bitowy `54 76 96`.

## Najważniejsza pułapka metodologiczna

SX1276 z odkręconą anteną nie jest stabilnym źródłem RF. Promieniuje przez przypadkowe sprzężenia płytki, przewodów i obudowy, więc poziom i jakość sygnału mogą zmieniać się bez poruszania urządzeń. Serie od 0% do kilkudziesięciu procent nie mogą służyć do porównywania progów AGC ani czułości.

Po dokręceniu anteny odbiór natychmiast ustabilizował się na 9/10 przy `-54..-56 dBm`. Potwierdza to działanie całej ścieżki, ale nie mierzy granicznej czułości, ponieważ sygnał jest mocny.

## Stan końcowy kodu i YAML

Zalecana konfiguracja:

```yaml
listen_mode: s1
rx_gain: boosted
has_tcxo: true
dio2_rf_switch: true
receiver_task_stack_size: 6144
```

Nie ma już opcji `agc_external_gain_db` ani `agc_first_power_threshold`. Zachowano diagnostyczny odczyt fabrycznych rejestrów AGC, korekcję erasure S1 i 24-bitowy sync.

## Wniosek i dalszy test

- Nie znaleziono programowego strojenia AGC, które wiarygodnie poprawia odbiór słabych S1 przez Heltec V4.
- Nie udowodniono laboratoryjnie różnicy czułości SX1276/SX1262; test bez anteny był niemiarodajny.
- Pozostaje istotna obserwacja terenowa: SX1276 widział słabą prawdziwą ramkę S1, której Heltec nie wykrył przed parserem.
- Dalsze porównanie musi używać normalnych anten, nieruchomych urządzeń i równoległych logów przez wiele godzin. Do pomiaru progu czułości potrzebny jest tłumik albo generator RF o kontrolowanym poziomie.

Powiązane commity:

- `00f46a0` — odzyskiwanie erasure Manchester per blok CRC;
- `7ea176e` — przywrócenie odpornego 24-bitowego sync;
- `fb12901` — usunięcie niejednoznacznych eksperymentów AGC.
