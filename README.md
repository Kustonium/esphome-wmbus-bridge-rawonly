# ESPHome wM-Bus Bridge RAW-only

🇬🇧 [English](#english) | 🇵🇱 [Polski](#polski)

```text
meter -> SX1262 / SX1276 / CC1101 -> ESPHome wmbus_radio -> MQTT RAW HEX -> backend / wmbusmeters / Home Assistant
```

---

## English

RAW-to-MQTT wireless M-Bus / wM-Bus radio bridge for ESPHome, focused on SX1262, SX1276 and experimental CC1101.

### What this project is

The ESP device is a radio receiver and MQTT publisher. It does not decode meter values on the ESP.

> 🌉 Together with the Home Assistant add-on [`homeassistant-wmbus-mqtt-bridge`](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge) this ESP is the **radio front-end of a distributed wM-Bus → Home Assistant gateway**: the ESP receives and forwards RAW HEX, the add-on decrypts and decodes. Unlike a monolithic wM-Bus gateway (radio + decoder in one box), the decode side is offloaded to Home Assistant — no meter drivers and no AES keys live on the ESP. The ESP also works standalone with any MQTT backend (Node-RED, a custom script), and the add-on accepts hex from any source (rtl-wmbus, another gateway, the replay tool) — the two cooperate, but neither depends on the other.

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

Most radiowy Wireless M-Bus / wM-Bus RAW-to-MQTT dla ESPHome, ukierunkowany na SX1262, SX1276 oraz eksperymentalnie CC1101.

### Czym jest ten projekt

Urządzenie ESP jest odbiornikiem radiowym i publisherem MQTT. Nie dekoduje wartości liczników na ESP.

> 🌉 Razem z dodatkiem Home Assistant [`homeassistant-wmbus-mqtt-bridge`](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge) ten ESP jest **radiowym frontendem rozproszonego gatewaya wM-Bus → Home Assistant**: ESP odbiera i przekazuje RAW HEX, add-on deszyfruje i dekoduje. W odróżnieniu od monolitycznej bramki wM-Bus (radio + dekoder w jednym pudełku) dekodowanie jest przeniesione na Home Assistant — na ESP nie ma driverów ani kluczy AES. ESP działa też samodzielnie z dowolnym backendem MQTT (Node-RED, własny skrypt), a add-on przyjmuje hex z dowolnego źródła (rtl-wmbus, inny gateway, narzędzie replay) — współpracują, ale żadna strona nie zależy od drugiej.

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
