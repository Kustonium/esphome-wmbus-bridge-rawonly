# Radio options / opcje radiowe

## Common / wspólne

| Radio | Wymagane piny | Opcjonalne | Uwagi |
|---|---|---|---|
| `SX1262` | `cs_pin`, `reset_pin`, `irq_pin` | `frequency`, `busy_pin`, TCXO, FEM, `long_gfsk_packets` | zalecany dla trudnego RF i długich ramek |
| `SX1276` | `cs_pin`, `reset_pin`, `irq_pin` | `frequency`, `busy_pin`, `sx1276_busy_ether_mode` | dobry dla spokojniejszych instalacji; ma mechanizm busy-ether |
| `CC1101` | `cs_pin`, `gdo0_pin`, `gdo2_pin` | `frequency` | eksperymentalny; wymaga `cc1101_allow_experimental: true`; single-IRQ nie jest wspierany |

## Listen mode frequency defaults / domyślne częstotliwości trybów

| `listen_mode` | Domyślna częstotliwość | Uwagi |
|---|---:|---|
| `t1` | `868.950 MHz` | T1 only |
| `c1` | `868.950 MHz` | C1 only |
| `both` | `868.950 MHz` | T1/C1 only; nie obejmuje S1 |
| `s1` | `868.300 MHz` | experimental S1 only |

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
| `long_gfsk_packets` | `false` | tryb długich pakietów GFSK |
| `clear_device_errors_on_boot` | `false` | czyści latched device errors po starcie |
| `publish_dev_err_after_clear` | `false` | publikuje wynik czyszczenia błędów |
| `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin` | brak | piny FEM, np. Heltec V4 |

## SX1276

| Opcja | Domyślnie | Opis |
|---|---:|---|
| `sx1276_busy_ether_mode` | `adaptive` | `normal`, `aggressive`, `adaptive` |

`busy_ether_state` w summary jest raportem tego mechanizmu i ma sens tylko dla SX1276.

`sx1276_busy_ether_mode` jest akceptowane przez schemat YAML także przy innych radiach, ale dla SX1262/CC1101 jest ignorowane bez błędu; w summary będzie `n/a`.

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
