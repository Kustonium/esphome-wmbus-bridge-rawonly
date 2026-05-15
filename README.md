# ESPHome wM-Bus Bridge RAW-only

RAW-only wireless M-Bus radio bridge for ESPHome.  
Most radiowy Wireless M-Bus RAW-only dla ESPHome.

```text
meter / licznik
  -> SX1262 / SX1276 / CC1101
  -> ESPHome wmbus_radio
  -> MQTT RAW HEX
  -> backend / wmbusmeters / Home Assistant
```

## What this project is / Czym jest ten projekt

The ESP device is a radio receiver and MQTT publisher. It does not decode meter values on the ESP.  
Urządzenie ESP jest odbiornikiem radiowym i publisherem MQTT. Nie dekoduje wartości liczników na ESP.

It does / Robi:

- receive T1/C1 frames and experimental S1 frames,  
  odbiera ramki T1/C1 oraz eksperymentalne ramki S1,
- validate and normalize telegrams,  
  waliduje i normalizuje telegramy,
- publish valid RAW HEX telegrams to MQTT,  
  publikuje poprawne telegramy RAW HEX do MQTT,
- publish RF diagnostics.  
  publikuje diagnostykę RF.

It does not / Nie robi:

- choose `wmbusmeters` drivers,  
  nie wybiera driverów `wmbusmeters`,
- decrypt AES payloads,  
  nie deszyfruje payloadów AES,
- create final meter-value sensors,  
  nie tworzy końcowych sensorów z wartościami liczników,
- replace `wmbusmeters`.  
  nie zastępuje `wmbusmeters`.

## Quick start / Szybki start

Use one of the YAML examples from `examples/`, then read the startup log before changing anything else.  
Użyj jednego z przykładów YAML z `examples/`, a potem najpierw przeczytaj log startowy, zanim zaczniesz zmieniać kolejne rzeczy.

Recommended path / Zalecana ścieżka:

1. Choose the matching board example from `examples/`.  
   Wybierz przykład z `examples/` pasujący do używanej płytki.
2. Use `topic_name` or omit it and let the component use `esphome.name`.  
   Użyj `topic_name` albo pomiń tę opcję i pozwól komponentowi użyć `esphome.name`.
3. Start with `listen_mode: t1` unless you know you need C1 or S1.  
   Zacznij od `listen_mode: t1`, chyba że wiesz, że potrzebujesz C1 albo S1.
4. Start with `diagnostic_mode: normal`.  
   Zacznij od `diagnostic_mode: normal`.
5. Check the boot sanity report and local `Have data / odebrano dane` logs before debugging MQTT/backend.  
   Sprawdź boot sanity report oraz lokalne logi `Have data / odebrano dane`, zanim zaczniesz debugować MQTT/backend.

## MQTT topic model / Model topiców MQTT

Recommended topic scheme / Zalecany schemat topiców:

```text
wmbus/<device>/telegram
wmbus/<device>/diag/summary
wmbus/<device>/diag/summary_15min
wmbus/<device>/diag/meter_snapshot
wmbus/<device>/diag/boot
```

The backend bridge should subscribe to:  
Backend bridge powinien subskrybować:

```text
wmbus/+/telegram
```

Do not manually build topic paths in normal YAML. Use `topic_name`, or omit it and let the component use `esphome.name`.  
W normalnym YAML-u nie składaj topiców ręcznie. Użyj `topic_name` albo pomiń tę opcję, a komponent użyje `esphome.name`.

```yaml
wmbus_radio:
  topic_name: "xiao_s3"
```

If MQTT is unavailable, radio reception continues and frames remain visible in local logs. MQTT publishing is skipped with a throttled warning.  
Jeżeli MQTT jest niedostępne, odbiór radiowy działa dalej, a ramki nadal są widoczne lokalnie w logach. Publikacja MQTT jest pomijana z ograniczanym czasowo ostrzeżeniem.

TLS, remote brokers and certificates belong to ESPHome's standard `mqtt:` configuration, not to `wmbus_radio`.  
TLS, zdalne brokery i certyfikaty należą do standardowej konfiguracji `mqtt:` ESPHome, a nie do `wmbus_radio`.

## Radio notes / Uwagi radiowe

### SX1262

SX1262 board-level options are explicit. The component does not guess board wiring.  
Opcje sprzętowe SX1262 są jawne. Komponent nie zgaduje okablowania płytki.

Common options / Typowe opcje:

```yaml
wmbus_radio:
  radio_type: SX1262
  has_tcxo: true
  dio2_rf_switch: true
  rx_gain: boosted
  long_gfsk_packets: true
```

At boot, the component prints a multiline SX1262 YAML sanity report. Missing `has_tcxo: true` on TCXO-based boards can still allow the radio to initialize, but RX may be completely silent. Disabled `long_gfsk_packets` in T1/both is reported as a risk for long T1 telegrams.  
Podczas startu komponent wypisuje wieloliniowy raport sanity YAML dla SX1262. Brak `has_tcxo: true` na płytkach z TCXO może nadal pozwolić na inicjalizację radia, ale RX może być całkowicie martwy. Wyłączone `long_gfsk_packets` w T1/both jest raportowane jako ryzyko dla długich telegramów T1.

### SX1276

Normal SX1276 boards do not need a TCXO option. Some boards expose a dedicated TCXO enable pin. Configure it explicitly only when your board documentation says so.  
Zwykłe płytki SX1276 nie wymagają opcji TCXO. Niektóre płytki mają osobny pin włączający TCXO. Ustaw go jawnie tylko wtedy, gdy wynika to z dokumentacji płytki.

Example for LILYGO T3 V3.0 TCXO OLED LoRa32:  
Przykład dla LILYGO T3 V3.0 TCXO OLED LoRa32:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

`tcxo_pin`, when configured, is driven HIGH before SX1276 radio initialization.  
Jeżeli `tcxo_pin` jest skonfigurowany, komponent ustawia go w stan HIGH przed inicjalizacją SX1276.

### CC1101

CC1101 support is experimental and requires explicit opt-in plus dual IRQ wiring. Single-IRQ CC1101 wiring is not supported.  
Obsługa CC1101 jest eksperymentalna i wymaga jawnego włączenia oraz podłączenia dwóch linii IRQ. Konfiguracja single-IRQ dla CC1101 nie jest wspierana.

```yaml
wmbus_radio:
  radio_type: CC1101
  cc1101_allow_experimental: true
  gdo0_pin: GPIOxx
  gdo2_pin: GPIOyy
```

## Documentation / Dokumentacja

Main documentation lives in `docs/`. Examples live in `examples/`.  
Główna dokumentacja jest w `docs/`. Przykłady są w `examples/`.

Start here / Zacznij tutaj:

- [`docs/START_HERE.md`](docs/START_HERE.md) / [`docs/START_HERE_PL.md`](docs/START_HERE_PL.md)
- [`docs/CONFIG_REFERENCE_MINIMAL.md`](docs/CONFIG_REFERENCE_MINIMAL.md)
- [`docs/RADIO_OPTIONS_MINIMAL.md`](docs/RADIO_OPTIONS_MINIMAL.md)
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) / [`docs/TROUBLESHOOTING_PL.md`](docs/TROUBLESHOOTING_PL.md)
- [`docs/DIAGNOSTIC.md`](docs/DIAGNOSTIC.md) / [`docs/DIAGNOSTIC_PL.md`](docs/DIAGNOSTIC_PL.md)
- [`examples/README.md`](examples/README.md) / [`examples/README_PL.md`](examples/README_PL.md)

Deeper notes / Głębsze notatki:

- [`docs/CHIP_SELECTION.md`](docs/CHIP_SELECTION.md) / [`docs/CHIP_SELECTION_PL.md`](docs/CHIP_SELECTION_PL.md)
- [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) / [`docs/BENCHMARKS_PL.md`](docs/BENCHMARKS_PL.md)
- [`docs/RX_PIPELINE.md`](docs/RX_PIPELINE.md) / [`docs/RX_PIPELINE_PL.md`](docs/RX_PIPELINE_PL.md)
- [`docs/RELEASE_NOTES.md`](docs/RELEASE_NOTES.md)

Older detailed README content was moved to:  
Starszy szczegółowy opis README przeniesiono do:

- [`docs/README_FULL.md`](docs/README_FULL.md) / [`docs/README_FULL_PL.md`](docs/README_FULL_PL.md)

## Support / Wsparcie

This project is intentionally RAW-only and is not a general ESPHome/Home Assistant support desk.  
Ten projekt jest celowo RAW-only i nie jest ogólnym helpdeskiem ESPHome/Home Assistant.

Before opening an issue, read [`SUPPORT.md`](SUPPORT.md).  
Przed otwarciem issue przeczytaj [`SUPPORT.md`](SUPPORT.md).
