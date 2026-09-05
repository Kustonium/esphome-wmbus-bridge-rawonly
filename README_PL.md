# ESPHome wM-Bus Bridge RAW-only

[English version](README.md)

[![Wydanie][release-shield]][releases]
[![Licencja][license-shield]][license]
![Obsługa SX1262][sx1262-shield]
![Obsługa SX1276][sx1276-shield]
![Obsługa CC1101][cc1101-shield]
![Obsługa LR1121][lr1121-shield]

[![CI][ci-shield]][ci]
[![Aktywność commitów][commits-shield]][commits]

<!-- Dynamiczne plakietki wskazują repozytorium stable, z którego instalują
     użytkownicy. Przy przenoszeniu dokumentacji z dev zachowaj te adresy.
     Nie dodawaj plakietek ze sztywno wpisanym rokiem utrzymania ani wersją
     ESPHome — kontrolę zgodności zapewnia cykliczne CI. -->

[release-shield]: https://img.shields.io/github/v/release/Kustonium/esphome-wmbus-bridge-rawonly
[releases]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/releases
[license-shield]: https://img.shields.io/github/license/Kustonium/esphome-wmbus-bridge-rawonly
[license]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/blob/main/LICENSE
[sx1262-shield]: https://img.shields.io/badge/SX1262-yes-green.svg
[sx1276-shield]: https://img.shields.io/badge/SX1276-yes-green.svg
[cc1101-shield]: https://img.shields.io/badge/CC1101-experimental-yellow.svg
[lr1121-shield]: https://img.shields.io/badge/LR1121-experimental-yellow.svg
[ci-shield]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/actions/workflows/ci.yml/badge.svg
[ci]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/actions/workflows/ci.yml
[commits-shield]: https://img.shields.io/github/commit-activity/y/Kustonium/esphome-wmbus-bridge-rawonly
[commits]: https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/commits/main

<a href="https://buymeacoffee.com/Kustonium"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Postaw mi kawę" height="41"></a>

```text
licznik -> SX1262 / SX1276 / CC1101 / LR1121 -> ESPHome wmbus_radio -> MQTT RAW HEX -> backend / wmbusmeters / Home Assistant
```

Most radiowy Wireless M-Bus / wM-Bus RAW-to-MQTT dla ESPHome, ukierunkowany na SX1262 i SX1276, z eksperymentalną obsługą CC1101 i LR1121.

> ✅ **Zweryfikowano na ESPHome 2026.7.0** — czyste kompilacje na wszystkich trzech płytkach testowych w CI.

## Czym jest ten projekt

Urządzenie ESP jest odbiornikiem radiowym i publisherem MQTT. Nie dekoduje wartości liczników na ESP.

> 🌉 Razem z dodatkiem Home Assistant [`homeassistant-wmbus-mqtt-bridge`](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge) ten ESP jest **radiowym frontendem rozproszonego gatewaya wM-Bus → Home Assistant**: ESP odbiera i przekazuje RAW HEX, add-on deszyfruje i dekoduje. W odróżnieniu od monolitycznej bramki wM-Bus (radio + dekoder w jednym pudełku) dekodowanie jest przeniesione na Home Assistant — na ESP nie ma driverów ani kluczy AES. ESP działa też samodzielnie z dowolnym backendem MQTT (Node-RED, własny skrypt), a add-on przyjmuje hex z dowolnego źródła (rtl-wmbus, inny gateway, narzędzie replay) — współpracują, ale żadna strona nie zależy od drugiej.

> 🧱 **Granica odpowiedzialności.** To firmware jest klientem MQTT: publikuje na temat i jego zakres tam się kończy. Broker — uwierzytelnianie, ACL, TLS, ekspozycja sieciowa oraz ewentualny mostek broker-broker dla instalacji zdalnych/rozproszonych (lokalizacja A → internet → lokalizacja B) — jest odpowiedzialnością operatora. Trzymaj broker w LAN, a do dostępu zdalnego użyj tunelu/VPN albo mostka brokera z TLS; nie wystawiaj portu 1883 do internetu.

Robi:

- odbiera ramki T1/C1 oraz eksperymentalne ramki S1,
- waliduje i normalizuje telegramy,
- publikuje poprawne telegramy RAW HEX do MQTT,
- publikuje diagnostykę RF.

Nie robi:

- nie wybiera driverów `wmbusmeters`,
- nie deszyfruje payloadów AES,
- nie tworzy końcowych sensorów z wartościami liczników,
- nie zastępuje `wmbusmeters`.

## Szybki start

Użyj jednego z przykładów YAML z `examples/`, a potem najpierw przeczytaj log startowy, zanim zaczniesz zmieniać kolejne rzeczy.

> **Wymagania.** Komponent buduje się i działa na frameworku ESP-IDF (używa go każdy przykład) i publikuje na bezwzględne tematy MQTT w postaci `wmbus/<topic_name>/telegram` — nie ma konfigurowalnego prefiksu tematu.

Zalecana ścieżka:

1. Wybierz przykład z `examples/` pasujący do używanej płytki.
2. Użyj `topic_name` albo pomiń tę opcję i pozwól komponentowi użyć `esphome.name`.
3. Zacznij od `listen_mode: t1`, chyba że wiesz, że potrzebujesz C1 albo S1.
4. Zacznij od `diagnostic_mode: normal`.
5. Sprawdź boot sanity report oraz lokalne logi `Have data`, zanim zaczniesz debugować MQTT/backend.

## Model topiców MQTT

Zalecany schemat topiców:

```text
wmbus/<device>/telegram
wmbus/<device>/rssi/<meter_id>      # tylko przy publish_rssi: true (domyślnie: false)
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
wmbus/<device>/diag/boot
```

Backend bridge powinien subskrybować:

```text
wmbus/+/telegram
```

W normalnym YAML-u nie składaj topiców ręcznie. Użyj `topic_name` albo pomiń tę opcję, a komponent użyje `esphome.name`.

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

Jeżeli MQTT jest niedostępne, odbiór radiowy działa dalej, a ramki nadal są widoczne lokalnie w logach. Publikacja MQTT jest pomijana z ograniczanym czasowo ostrzeżeniem.

TLS, zdalne brokery i certyfikaty należą do standardowej konfiguracji `mqtt:` ESPHome, a nie do `wmbus_radio`.

## Uwagi radiowe

### SX1262

Opcje sprzętowe SX1262 są jawne. Komponent nie zgaduje okablowania płytki.

Typowe opcje:

```yaml
wmbus_radio:
  radio_type: SX1262
  has_tcxo: true
  dio2_rf_switch: true
  rx_gain: boosted
  long_gfsk_packets: false
```

Podczas startu komponent wypisuje wieloliniowy raport sanity YAML dla SX1262. Brak `has_tcxo: true` na płytkach z TCXO może nadal pozwolić na inicjalizację radia, ale RX może być całkowicie martwy. Domyślne `long_gfsk_packets: false` zachowuje czułość; włącz tę opcję tylko dla ramek powyżej około 150 bajtów zdekodowanych. Tryb strumieniowy kosztował w opisanych pomiarach około 7 dB przy słabym sygnale.

Część modułów bramkuje swój tor antenowy zewnętrznym wyprowadzeniem i bez niego pracuje z czułością niższą o około 30 dB. Moduł Seeed Wio-SX1262 jest jednym z nich — w zestawie z XIAO ESP32-S3 tym wyprowadzeniem jest `GPIO38`:

```yaml
wmbus_radio:
  radio_type: SX1262
  rf_sw_pin: GPIO38
```

To nie jest ta sama opcja co `dio2_rf_switch` i potrzebne są obie: DIO2 wybiera kierunek TX/RX wewnątrz układu, a `rf_sw_pin` decyduje, czy przełącznik RF modułu w ogóle przewodzi. Płytki Heltec V3/V4/V4-R8 tej opcji nie wymagają — korzystają z wyprowadzeń `fem_*`.

### SX1276

Zwykłe płytki SX1276 nie wymagają opcji TCXO. Niektóre płytki mają osobny pin włączający TCXO. Ustaw go jawnie tylko wtedy, gdy wynika to z dokumentacji płytki.

Przykład dla LILYGO T3 V3.0 TCXO OLED LoRa32:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

Jeżeli `tcxo_pin` jest skonfigurowany, komponent ustawia go w stan HIGH przed inicjalizacją SX1276.

### CC1101

Obsługa CC1101 jest eksperymentalna i wymaga jawnego włączenia oraz podłączenia dwóch linii IRQ. Konfiguracja single-IRQ dla CC1101 nie jest wspierana.

```yaml
wmbus_radio:
  radio_type: CC1101
  cc1101_allow_experimental: true
  gdo0_pin: GPIOxx
  gdo2_pin: GPIOyy
```

Rewizje układu: akceptowane są moduły zgłaszające zarówno `VERSION=0x14`, jak i `VERSION=0x04`. Bajt rewizji trafia do logu, ale nie blokuje startu — nierozpoznana wartość daje ostrzeżenie, a odbiornik i tak rusza. O używalności radia decyduje autotest rejestrów (mapowanie GDO, tryb pakietu, profil RF), a całkowicie milcząca magistrala SPI (`VERSION` równe `0x00` albo `0xFF`) nadal przerywa uruchomienie.

### LR1121

Obsługa LR1121 jest eksperymentalna i wymaga jawnego włączenia. Na płytce
Waveshare ESP32-S3-LR1121-HF (SKU 34011) zdekodowała realny ruch T1, C1 i S1.

Chodziła już tygodniami obok czterech innych odbiorników w jednym mieszkaniu,
więc brakujące porównanie zostało zrobione. Nie wypada dla niej korzystnie:
w oknie 12 h usłyszała **48 liczników wobec 113 u T-Beama na SX1262**, przy
identycznej antenie 10 cm, tym samym firmwarze i tym samym oknie. Na stanowisku
tłumikowym była pierwszą płytką, która straciła sygnał całkowicie, podczas gdy
dwie płytki SX1262 dekodowały jeszcze 92,9% ramek. Zamiana anten między nimi
wykluczyła antenę: LR1121 przegrywał, mając tę lepszą.

Mechanizm widać w licznikach. Ma **najwyższą konwersję z całej stawki (80%
wyzwoleń odbiornika zamieniło się w ramki)** i zdecydowanie najmniej wyzwoleń.
To najostrożniejszy odbiornik z zestawu - rzadko podejmuje próbę przy słabym
starcie i rzadko ją marnuje. Dlatego słyszy mniej: przy tym obciążeniu to
gotowość do próbowania kupuje zasięg.

To nadal jedna płytka, jedno mieszkanie, jedna osoba, i nic z tego nie oddziela
układu od tego sterownika. Jawne włączenie zostaje z tego powodu, a nie dlatego,
że sterownik jest nieprzetestowany.

```yaml
wmbus_radio:
  radio_type: LR1121
  lr1121_allow_experimental: true
  cs_pin: GPIO42
  reset_pin: GPIO39
  irq_pin: GPIO38
  busy_pin: GPIO41
  tcxo_voltage: 3.0v
  payload_length: 255
```

`tcxo_voltage: 3.0v` jest zmierzone, nie założone: przy `1.8v` układ zgłasza
`HF_XOSC_START` i nie dochodzi do odbioru. `payload_length: 255` jest potrzebne,
bo ramki NES mają 245 bajtów surowych.

`HF_XOSC_START` w logu startowym jest normalny i nie jest usterką — zatrzaskuje się
przy wejściu w `STDBY_XOSC`, obie kalibracje po nim wracają czyste, a sterownik
tłumaczy to w logu.

Płytka nie ma mostka USB-UART, więc `logger:` wymaga
`hardware_uart: USB_SERIAL_JTAG`, inaczej na konsoli nie pojawi się nic. Ma też
więcej niż jedno gniazdo u.FL, a torem sub-GHz jest tylko jedno z nich — złe
gniazdo daje idealną ciszę bez żadnego błędu.

## Dokumentacja

Główna dokumentacja jest w `docs/`. Przykłady są w `examples/`.

Zacznij tutaj:

- [`docs/START_HERE_PL.md`](docs/START_HERE_PL.md)
- [`docs/CONFIG_REFERENCE_MINIMAL_PL.md`](docs/CONFIG_REFERENCE_MINIMAL_PL.md)
- [`docs/RADIO_OPTIONS_MINIMAL_PL.md`](docs/RADIO_OPTIONS_MINIMAL_PL.md)
- [`docs/ON_FRAME_PL.md`](docs/ON_FRAME_PL.md)
- [`docs/TROUBLESHOOTING_PL.md`](docs/TROUBLESHOOTING_PL.md)
- [`docs/DIAGNOSTIC_PL.md`](docs/DIAGNOSTIC_PL.md)
- [`examples/README_PL.md`](examples/README_PL.md)

Głębsze notatki:

- [`docs/CHIP_SELECTION_PL.md`](docs/CHIP_SELECTION_PL.md)
- [`docs/BENCHMARKS_PL.md`](docs/BENCHMARKS_PL.md)
- [`docs/RX_PIPELINE_PL.md`](docs/RX_PIPELINE_PL.md)
- Historia zmian: [GitHub Releases](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/releases)

Starszy szczegółowy opis README przeniesiono do [`docs/README_FULL_PL.md`](docs/README_FULL_PL.md).

## Wsparcie

Ten projekt jest celowo RAW-only i nie jest ogólnym helpdeskiem ESPHome/Home Assistant.

Przed otwarciem issue przeczytaj [`SUPPORT_PL.md`](SUPPORT_PL.md).
