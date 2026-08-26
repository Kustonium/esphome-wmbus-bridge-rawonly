# ESPHome wM-Bus Bridge RAW-only

[![Release][release-shield]][releases]
[![License][license-shield]][license]
![Supports SX1262][sx1262-shield]
![Supports SX1276][sx1276-shield]
![Supports CC1101][cc1101-shield]
![Supports LR1121][lr1121-shield]

[![CI][ci-shield]][ci]
[![Commit activity][commits-shield]][commits]

<!-- Every dynamic badge reads the stable repository, which is the one users
     install from, and this README is copied to it verbatim by promote.yml — so
     no badge here may point at the dev repository. The radio badges are static
     but factual (see the compatibility section below). Deliberately absent:
     shields.io "maintenance/yes/<year>", which flips itself to a red
     "maintained: no!" as soon as the hard-coded year rolls over, and any badge
     naming a specific ESPHome version, which would go stale silently — the CI
     badge proves current compatibility instead, because CI builds against
     unpinned ESPHome on a nightly schedule. -->

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

<a href="https://buymeacoffee.com/Kustonium"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" height="41"></a>

🇬🇧 [English](#english) | 🇵🇱 [Polski](#polski)

```text
meter -> SX1262 / SX1276 / CC1101 / LR1121 -> ESPHome wmbus_radio -> MQTT RAW HEX -> backend / wmbusmeters / Home Assistant
```

---

## English

RAW-to-MQTT wireless M-Bus / wM-Bus radio bridge for ESPHome, focused on SX1262 and SX1276, with experimental CC1101 and LR1121 support.

> ✅ **Verified on ESPHome 2026.7.0** — clean builds on all three test boards in CI.

### What this project is

The ESP device is a radio receiver and MQTT publisher. It does not decode meter values on the ESP.

> 🌉 Together with the Home Assistant add-on [`homeassistant-wmbus-mqtt-bridge`](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge) this ESP is the **radio front-end of a distributed wM-Bus → Home Assistant gateway**: the ESP receives and forwards RAW HEX, the add-on decrypts and decodes. Unlike a monolithic wM-Bus gateway (radio + decoder in one box), the decode side is offloaded to Home Assistant — no meter drivers and no AES keys live on the ESP. The ESP also works standalone with any MQTT backend (Node-RED, a custom script), and the add-on accepts hex from any source (rtl-wmbus, another gateway, the replay tool) — the two cooperate, but neither depends on the other.

> 🧱 **Responsibility boundary.** This firmware is an MQTT client: it publishes to a topic and its scope ends there. The broker — authentication, ACLs, TLS, network exposure and any broker-to-broker bridging for remote/distributed setups (site A → internet → site B) — is the operator's responsibility. Keep the broker on your LAN and use a tunnel/VPN or TLS broker bridging for remote access; do not expose port 1883 to the internet.

It does:

- receive T1/C1 frames and experimental S1 frames,
- validate and normalize telegrams,
- publish valid RAW HEX telegrams to MQTT,
- publish RF diagnostics.

It does not:

- choose `wmbusmeters` drivers,
- decrypt AES payloads,
- create final meter-value sensors,
- replace `wmbusmeters`.

### Quick start

Use one of the YAML examples from `examples/`, then read the startup log before changing anything else.

> **Requirements.** This component builds and runs on the ESP-IDF framework (every example uses it) and publishes to absolute MQTT topics of the form `wmbus/<topic_name>/telegram` — there is no configurable topic prefix.

Recommended path:

1. Choose the matching board example from `examples/`.
2. Use `topic_name` or omit it and let the component use `esphome.name`.
3. Start with `listen_mode: t1` unless you know you need C1 or S1.
4. Start with `diagnostic_mode: normal`.
5. Check the boot sanity report and local `Have data` logs before debugging MQTT/backend.

### MQTT topic model

Recommended topic scheme:

```text
wmbus/<device>/telegram
wmbus/<device>/rssi/<meter_id>      # only with publish_rssi: true (default: false)
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
wmbus/<device>/diag/boot
```

The backend bridge should subscribe to:

```text
wmbus/+/telegram
```

Do not manually build topic paths in normal YAML. Use `topic_name`, or omit it and let the component use `esphome.name`.

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

If MQTT is unavailable, radio reception continues and frames remain visible in local logs. MQTT publishing is skipped with a throttled warning.

TLS, remote brokers and certificates belong to ESPHome's standard `mqtt:` configuration, not to `wmbus_radio`.

### Radio notes

#### SX1262

SX1262 board-level options are explicit. The component does not guess board wiring.

Common options:

```yaml
wmbus_radio:
  radio_type: SX1262
  has_tcxo: true
  dio2_rf_switch: true
  rx_gain: boosted
  long_gfsk_packets: true
```

At boot, the component prints a multiline SX1262 YAML sanity report. Missing `has_tcxo: true` on TCXO-based boards can still allow the radio to initialize, but RX may be completely silent. Disabled `long_gfsk_packets` in T1/both is reported as a risk for long T1 telegrams.

Some modules gate their antenna path behind an external pin and stay roughly 30 dB deaf without it. The Seeed Wio-SX1262 is one of them - on the XIAO ESP32S3 kit that pin is `GPIO38`:

```yaml
wmbus_radio:
  radio_type: SX1262
  rf_sw_pin: GPIO38
```

This is not the same option as `dio2_rf_switch`, and both are needed: DIO2 selects the TX/RX direction inside the chip, `rf_sw_pin` decides whether the module's RF switch conducts at all. Heltec V3/V4/V4-R8 do not need it - they use the `fem_*` pins.

#### SX1276

Normal SX1276 boards do not need a TCXO option. Some boards expose a dedicated TCXO enable pin. Configure it explicitly only when your board documentation says so.

Example for LILYGO T3 V3.0 TCXO OLED LoRa32:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

`tcxo_pin`, when configured, is driven HIGH before SX1276 radio initialization.

#### CC1101

CC1101 support is experimental and requires explicit opt-in plus dual IRQ wiring. Single-IRQ CC1101 wiring is not supported.

```yaml
wmbus_radio:
  radio_type: CC1101
  cc1101_allow_experimental: true
  gdo0_pin: GPIOxx
  gdo2_pin: GPIOyy
```

Silicon revisions: modules reporting `VERSION=0x14` and `VERSION=0x04` are both accepted. The revision byte is logged, but it does not gate startup — an unrecognised value produces a warning and the receiver still starts. What decides whether the radio is usable is the register self-check (GDO mapping, packet mode, RF profile), and a completely silent SPI bus (`VERSION` reading `0x00` or `0xFF`) still fails setup.

#### LR1121

LR1121 support is experimental and requires explicit opt-in. It has decoded real
T1, C1 and S1 traffic on the Waveshare ESP32-S3-LR1121-HF board (SKU 34011), but
it has not run for weeks and has never been compared against another radio in the
same position.

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

`tcxo_voltage: 3.0v` is measured, not assumed: at `1.8v` the chip reports
`HF_XOSC_START` and never reaches the receive path. `payload_length: 255` is
needed because NES frames are 245 raw bytes.

`HF_XOSC_START` in the boot log is normal and is not a fault — it latches when the
chip enters `STDBY_XOSC`, both calibrations afterwards come back clean, and the
driver explains this in the log.

This board has no USB-serial bridge, so `logger:` needs
`hardware_uart: USB_SERIAL_JTAG` or nothing will appear on the console. It also
has more than one u.FL socket, and only one of them is the sub-GHz path — the
wrong socket gives perfect silence with no error anywhere.

### Documentation

Main documentation lives in `docs/`. Examples live in `examples/`.

Start here:

- [`docs/START_HERE.md`](docs/START_HERE.md)
- [`docs/CONFIG_REFERENCE_MINIMAL.md`](docs/CONFIG_REFERENCE_MINIMAL.md)
- [`docs/RADIO_OPTIONS_MINIMAL.md`](docs/RADIO_OPTIONS_MINIMAL.md)
- [`docs/ON_FRAME.md`](docs/ON_FRAME.md)
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)
- [`docs/DIAGNOSTIC.md`](docs/DIAGNOSTIC.md)
- [`examples/README.md`](examples/README.md)

Deeper notes:

- [`docs/CHIP_SELECTION.md`](docs/CHIP_SELECTION.md)
- [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md)
- [`docs/RX_PIPELINE.md`](docs/RX_PIPELINE.md)
- Release notes: [GitHub Releases](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/releases)

Older detailed README content was moved to [`docs/README_FULL.md`](docs/README_FULL.md).

### Support

This project is intentionally RAW-only and is not a general ESPHome/Home Assistant support desk.

Before opening an issue, read [`SUPPORT.md`](SUPPORT.md).

---

## Polski

Most radiowy Wireless M-Bus / wM-Bus RAW-to-MQTT dla ESPHome, ukierunkowany na SX1262 i SX1276, z eksperymentalną obsługą CC1101 i LR1121.

> ✅ **Zweryfikowano na ESPHome 2026.7.0** — czyste kompilacje na wszystkich trzech płytkach testowych w CI.

### Czym jest ten projekt

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

### Szybki start

Użyj jednego z przykładów YAML z `examples/`, a potem najpierw przeczytaj log startowy, zanim zaczniesz zmieniać kolejne rzeczy.

> **Wymagania.** Komponent buduje się i działa na frameworku ESP-IDF (używa go każdy przykład) i publikuje na bezwzględne tematy MQTT w postaci `wmbus/<topic_name>/telegram` — nie ma konfigurowalnego prefiksu tematu.

Zalecana ścieżka:

1. Wybierz przykład z `examples/` pasujący do używanej płytki.
2. Użyj `topic_name` albo pomiń tę opcję i pozwól komponentowi użyć `esphome.name`.
3. Zacznij od `listen_mode: t1`, chyba że wiesz, że potrzebujesz C1 albo S1.
4. Zacznij od `diagnostic_mode: normal`.
5. Sprawdź boot sanity report oraz lokalne logi `Have data`, zanim zaczniesz debugować MQTT/backend.

### Model topiców MQTT

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

### Uwagi radiowe

#### SX1262

Opcje sprzętowe SX1262 są jawne. Komponent nie zgaduje okablowania płytki.

Typowe opcje:

```yaml
wmbus_radio:
  radio_type: SX1262
  has_tcxo: true
  dio2_rf_switch: true
  rx_gain: boosted
  long_gfsk_packets: true
```

Podczas startu komponent wypisuje wieloliniowy raport sanity YAML dla SX1262. Brak `has_tcxo: true` na płytkach z TCXO może nadal pozwolić na inicjalizację radia, ale RX może być całkowicie martwy. Wyłączone `long_gfsk_packets` w T1/both jest raportowane jako ryzyko dla długich telegramów T1.

Część modułów bramkuje swój tor antenowy zewnętrznym wyprowadzeniem i bez niego pracuje z czułością niższą o około 30 dB. Moduł Seeed Wio-SX1262 jest jednym z nich — w zestawie z XIAO ESP32-S3 tym wyprowadzeniem jest `GPIO38`:

```yaml
wmbus_radio:
  radio_type: SX1262
  rf_sw_pin: GPIO38
```

To nie jest ta sama opcja co `dio2_rf_switch` i potrzebne są obie: DIO2 wybiera kierunek TX/RX wewnątrz układu, a `rf_sw_pin` decyduje, czy przełącznik RF modułu w ogóle przewodzi. Płytki Heltec V3/V4/V4-R8 tej opcji nie wymagają — korzystają z wyprowadzeń `fem_*`.

#### SX1276

Zwykłe płytki SX1276 nie wymagają opcji TCXO. Niektóre płytki mają osobny pin włączający TCXO. Ustaw go jawnie tylko wtedy, gdy wynika to z dokumentacji płytki.

Przykład dla LILYGO T3 V3.0 TCXO OLED LoRa32:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

Jeżeli `tcxo_pin` jest skonfigurowany, komponent ustawia go w stan HIGH przed inicjalizacją SX1276.

#### CC1101

Obsługa CC1101 jest eksperymentalna i wymaga jawnego włączenia oraz podłączenia dwóch linii IRQ. Konfiguracja single-IRQ dla CC1101 nie jest wspierana.

```yaml
wmbus_radio:
  radio_type: CC1101
  cc1101_allow_experimental: true
  gdo0_pin: GPIOxx
  gdo2_pin: GPIOyy
```

Rewizje układu: akceptowane są moduły zgłaszające zarówno `VERSION=0x14`, jak i `VERSION=0x04`. Bajt rewizji trafia do logu, ale nie blokuje startu — nierozpoznana wartość daje ostrzeżenie, a odbiornik i tak rusza. O używalności radia decyduje autotest rejestrów (mapowanie GDO, tryb pakietu, profil RF), a całkowicie milcząca magistrala SPI (`VERSION` równe `0x00` albo `0xFF`) nadal przerywa uruchomienie.

#### LR1121

Obsługa LR1121 jest eksperymentalna i wymaga jawnego włączenia. Na płytce
Waveshare ESP32-S3-LR1121-HF (SKU 34011) zdekodowała realny ruch T1, C1 i S1, ale
nie chodziła tygodniami i nigdy nie była porównana z innym radiem w tym samym
miejscu.

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

### Dokumentacja

Główna dokumentacja jest w `docs/`. Przykłady są w `examples/`.

Zacznij tutaj:

- [`docs/START_HERE_PL.md`](docs/START_HERE_PL.md)
- [`docs/CONFIG_REFERENCE_MINIMAL.md`](docs/CONFIG_REFERENCE_MINIMAL.md)
- [`docs/RADIO_OPTIONS_MINIMAL.md`](docs/RADIO_OPTIONS_MINIMAL.md)
- [`docs/ON_FRAME_PL.md`](docs/ON_FRAME_PL.md)
- [`docs/TROUBLESHOOTING_PL.md`](docs/TROUBLESHOOTING_PL.md)
- [`docs/DIAGNOSTIC_PL.md`](docs/DIAGNOSTIC_PL.md)
- [`examples/README_PL.md`](examples/README_PL.md)

Głębsze notatki:

- [`docs/CHIP_SELECTION_PL.md`](docs/CHIP_SELECTION_PL.md)
- [`docs/BENCHMARKS_PL.md`](docs/BENCHMARKS_PL.md)
- [`docs/RX_PIPELINE_PL.md`](docs/RX_PIPELINE_PL.md)
- Release notes: [GitHub Releases](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/releases)

Starszy szczegółowy opis README przeniesiono do [`docs/README_FULL_PL.md`](docs/README_FULL_PL.md).

### Wsparcie

Ten projekt jest celowo RAW-only i nie jest ogólnym helpdeskiem ESPHome/Home Assistant.

Przed otwarciem issue przeczytaj [`SUPPORT.md`](SUPPORT.md).
