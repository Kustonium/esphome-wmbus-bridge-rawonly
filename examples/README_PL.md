# Przykłady

[English version](README.md)

Publiczne przykłady są dla płytek SX1262, SX1276 i CC1101.

Obsługa CC1101 jest dostępna, ale nadal eksperymentalna. Wymaga jawnego włączenia w YAML oraz poprawnego podłączenia GDO0/GDO2. Ten przykład jest przeznaczony do zastosowań zaawansowanych/testowych, a nie jako rekomendowana ścieżka dla zwykłych użytkowników.

## Model topiców

Przykłady używają automatycznego modelu MQTT.

Jeżeli `topic_name` jest pominięty, komponent używa `esphome.name` i generuje:

```text
wmbus/<esphome.name>/telegram
wmbus/<esphome.name>/diag/...
```

Dlatego dodatek bridge w Home Assistant powinien subskrybować:

```text
wmbus/+/telegram
```

Nie kopiuj starych `telegram_topic` / `diagnostic_topic`, chyba że celowo potrzebujesz legacy/manual override.

Jeżeli broker MQTT jest niedostępny w runtime, odbiór radiowy działa dalej, a ramki nadal są widoczne lokalnie w logach. Publikacja MQTT jest pomijana z ograniczanym czasowo ostrzeżeniem.

## Tryby nasłuchu i częstotliwość

Przykłady startują od:

```yaml
listen_mode: t1
```

Dostępne tryby odbioru:

```text
t1    -> tylko T1, domyślnie 868.950 MHz
c1    -> tylko C1, domyślnie 868.950 MHz
both  -> tylko T1/C1, domyślnie 868.950 MHz
s1    -> tylko S1, domyślnie 868.300 MHz
```

`both` oznacza tylko T1/C1. S1 jest osobnym trybem odbioru i musi być wybrany jawnie przez `listen_mode: s1`.

`frequency:` jest opcjonalne. Używaj tego tylko wtedy, gdy celowo chcesz nadpisać domyślną częstotliwość trybu, np. do testów kompatybilności:

```yaml
wmbus_radio:
  listen_mode: s1
  frequency: 868.36
```

Jeżeli poprawny telegram S1 zostanie odebrany, komponent opublikuje surowy telegram do MQTT tak samo jak T1/C1. Dekodowanie wartości licznika nadal zależy od backendu, drivera i klucza szyfrowania.

## Opcje sprzętowe SX1262

Przykłady SX1262 używają jawnych opcji sprzętowych zamiast zgadywania okablowania płytki:

```yaml
has_tcxo: true
dio2_rf_switch: true
rx_gain: boosted
long_gfsk_packets: true
```

Komponent wypisuje podczas startu wieloliniowy raport sanity YAML dla SX1262. Ryzykowne ustawienia, takie jak brak `has_tcxo: true` na płytkach z TCXO albo wyłączone `long_gfsk_packets` w trybach T1/both, są raportowane jako warningi, ale nie blokują startu.

## Płytki SX1276 z TCXO

Zwykłe płytki SX1276 nie wymagają opcji TCXO.

Niektóre płytki SX1276 mają osobny pin włączający TCXO. Dla takich płytek skonfiguruj go jawnie:

```yaml
wmbus_radio:
  radio_type: SX1276
  tcxo_pin: GPIO12
```

Przykład: LILYGO T3 V3.0 TCXO OLED LoRa32 używa `tcxo_pin: GPIO12`.

Komponent nie wykrywa automatycznie okablowania płytki. Przed ustawieniem `tcxo_pin` sprawdź schemat płytki albo dokumentację producenta.

## Model diagnostyki

Przykłady używają:

```yaml
diagnostic_mode: normal
```

To daje:
- globalne `summary`,
- `summary_15min`,
- `meter_snapshot` dla ID wpisanych w `highlight_meters`.

Dla cichszej pracy użyj:

```yaml
diagnostic_mode: low
```

Do głębszego debugowania użyj:

```yaml
diagnostic_mode: debug
```

## Nazwy plików

Każda płytka ma dwie wersje:

- `*_clean.yaml` — minimalna praktyczna konfiguracja,
- `*_commented.yaml` — ta sama idea z dwujęzycznymi komentarzami i wyjaśnieniami.
