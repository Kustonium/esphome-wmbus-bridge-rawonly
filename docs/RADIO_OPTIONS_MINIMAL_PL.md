# Opcje radiowe

[English version](RADIO_OPTIONS_MINIMAL.md)

## Wspólne

| Radio | Wymagane piny | Opcjonalne | Uwagi |
|---|---|---|---|
| `SX1262` | `cs_pin`, `reset_pin`, `irq_pin`, `busy_pin` | `frequency`, TCXO, FEM, bramka RF switch, `long_gfsk_packets` | zalecany dla trudnego RF i długich ramek |
| `SX1276` | `cs_pin`, `reset_pin`, `irq_pin` | `frequency`, `busy_pin`, `sx1276_busy_ether_mode`, `tcxo_pin` | dobry dla spokojniejszych instalacji; ma mechanizm busy-ether; `tcxo_pin` tylko dla płytek z osobnym TCXO enable |
| `CC1101` | `cs_pin`, `gdo0_pin`, `gdo2_pin` | `frequency` | eksperymentalny; wymaga `cc1101_allow_experimental: true`; single-IRQ nie jest wspierany |
| `LR1121` | `cs_pin`, `reset_pin`, `irq_pin`, `busy_pin` | `frequency`, `tcxo_voltage`, `tcxo_startup_ticks`, `payload_length` | eksperymentalny; wymaga `lr1121_allow_experimental: true`; odbiera T1/C1/S1 na sprzęcie Waveshare HF |

## Domyślne częstotliwości trybów

| `listen_mode` | Domyślna częstotliwość | Uwagi |
|---|---:|---|
| `t1` | `868.950 MHz` | tylko T1 |
| `c1` | `868.950 MHz` | tylko C1 |
| `both` | `868.950 MHz` | tylko T1/C1; nie obejmuje S1 |
| `s1` | `868.300 MHz` | tylko eksperymentalny S1 |

`frequency:` można podać przy każdym radiu jako jawne nadpisanie domyślnej częstotliwości trybu. Typowy przykład diagnostyczny S1:

```yaml
wmbus_radio:
  radio_type: SX1262
  listen_mode: s1
  frequency: 868.36
```

S1 używa innego profilu RF niż T1/C1, dlatego nie jest łączony z `both`.

## SX1262

| Opcja | Domyślnie | Opis |
|---|---:|---|
| `has_tcxo` | `false` | włącz dla modułów z TCXO, np. część Heltec |
| `dio2_rf_switch` / `rf_switch` | `true` | sterowanie RF switch przez DIO2 |
| `rx_gain` | `boosted` | `boosted` albo `power_saving` |
| `long_gfsk_packets` | `false` | tryb długich pakietów GFSK; **kosztuje ok. 7 dB przy słabym sygnale** — włączać tylko, gdy realnie odbierasz ramki powyżej ~150 bajtów zdekodowanych |
| `min_preamble_bits` | `16` | próg detektora preambuły; dla T1 **16 to maksimum**, `8` kosztuje ok. 16% liczników (patrz `CONFIG_REFERENCE_MINIMAL_PL.md`) |
| `clear_device_errors_on_boot` | `false` | czyści latched device errors po starcie |
| `publish_dev_err_after_clear` | `false` | publikuje wynik czyszczenia błędów |
| `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin` | brak | piny FEM, np. Heltec V4 |
| `rf_sw_pin` | brak | bramka wewnętrznego przełącznika RF modułu; wymagane na XIAO ESP32-S3 + Wio-SX1262 (`GPIO38`) |

### `rf_sw_pin` a `dio2_rf_switch`

To dwie różne rzeczy i na płytkach, które tego wymagają, potrzebne są obie.

`dio2_rf_switch` odnosi się do wyprowadzenia DIO2 samego układu SX1262 i wybiera **kierunek** TX/RX. `rf_sw_pin` to zewnętrzne wyprowadzenie GPIO hosta, które otwiera **bramkę** przełącznika RF w module — decyduje, czy przełącznik w ogóle przewodzi.

Moduł Seeed Wio-SX1262 wyprowadza tę bramkę na pinie 1 (`RF_SW`); w zestawie z XIAO ESP32-S3 sygnał trafia na `GPIO38`:

```yaml
wmbus_radio:
  radio_type: SX1262
  rf_sw_pin: GPIO38
```

Bez tej opcji odbiornik pracuje z czułością niższą o około 30 dB. Objawem nie jest cisza: ramki nadal się dekodują i `DIAG hint` raportuje `GOOD`, ale jest ich kilka razy mniej, a wszystkie wartości RSSI leżą w wąskim paśmie tuż nad progiem czułości.

Nie steruj tym wyprowadzeniem akcją `on_boot` na wyjściu `gpio` — patrz [Rozwiązywanie problemów](TROUBLESHOOTING_PL.md), sekcja o odbiorniku słyszącym mało liczników.

Płytki Heltec V3 / V4 / V4-R8 tej opcji nie wymagają; ewentualne ustawienia `fem_*` sprawdź w przykładzie dla danej płytki.

## SX1276

| Opcja | Domyślnie | Opis |
|---|---:|---|
| `sx1276_busy_ether_mode` | `normal` | `normal`, `aggressive`, `adaptive` |
| `tcxo_pin` | brak | opcjonalny pin TCXO enable; tylko dla SX1276 |

`busy_ether_state` w summary jest raportem mechanizmu busy-ether i ma sens tylko dla SX1276.

`sx1276_busy_ether_mode` jest akceptowane przez schemat YAML także przy innych radiach, ale dla SX1262/CC1101/LR1121 jest ignorowane bez błędu; w summary będzie `n/a`.

`tcxo_pin` jest opcją jawną dla płytek SX1276 z osobnym pinem TCXO enable. Jeśli jest ustawiony, komponent ustawia ten pin w stan HIGH przed inicjalizacją radia. Zwykłe płytki SX1276 nie wymagają tej opcji.

Przykład dla LILYGO T3 V3.0 TCXO OLED LoRa32:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

Komponent nie wykrywa automatycznie modelu płytki ani okablowania TCXO. Sprawdź schemat płytki albo dokumentację producenta.

## CC1101

Minimalny schemat:

```yaml
wmbus_radio:
  - radio_type: CC1101
    cc1101_allow_experimental: true
    cs_pin: GPIO4
    gdo0_pin: GPIO3
    gdo2_pin: GPIO2
    frequency: 868.95
```

Nie kopiować konfiguracji CC1101 z projektów single-IRQ. Ten komponent wymaga osobno GDO0 i GDO2.

## LR1121

Minimalny schemat (Waveshare ESP32-S3-LR1121-HF, SKU 34011):

```yaml
wmbus_radio:
  - radio_type: LR1121
    lr1121_allow_experimental: true
    cs_pin: GPIO42
    reset_pin: GPIO39
    irq_pin: GPIO38
    busy_pin: GPIO41
    tcxo_voltage: 3.0v
    payload_length: 255
```

Trzy rzeczy, bez których ta płytka wygląda na martwą, choć działa:

- **`tcxo_voltage: 3.0v`** — zmierzone. Przy `1.8v` układ zgłasza `HF_XOSC_START`
  i nie dochodzi do odbioru. Pakiet producenta podaje obie wartości dla tej samej
  płytki; obowiązuje ta zweryfikowana na sprzęcie.
- **`payload_length: 255`** — ramki NES mają 245 bajtów surowych. Niższa wartość
  je utnie.
- **Gniazdo antenowe.** Płytka ma więcej niż jedno gniazdo u.FL: WiFi ESP32,
  port 2,4 GHz LR1121 i dopiero jedno z nich jest torem sub-GHz przez przełącznik
  RF. Złe gniazdo daje idealną ciszę bez żadnego błędu.

Ta płytka nie ma mostka USB-UART — konsola idzie przez natywne USB ESP32-S3, więc
`logger:` wymaga `hardware_uart: USB_SERIAL_JTAG`, inaczej log nie pojawi się nigdzie.

Komunikat `HF_XOSC_START` przy starcie **jest normalny i nie jest usterką** —
zapala się przy wejściu w `STDBY_XOSC`, a obie kalibracje po nim wracają czyste.
Sterownik tłumaczy to w logu.
