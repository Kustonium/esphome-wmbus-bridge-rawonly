# Przykłady

[English version](README.md)

Publiczne przykłady są tylko dla płytek SX1262,SX1276,CC1101.

Obsługa CC1101 jest dostępna, ale nadal eksperymentalna. Wymaga jawnego włączenia w YAML oraz poprawnego podłączenia GDO0/GDO2. Ten przykład jest przeznaczony do zastosowań zaawansowanych/testowych, a nie jako rekomendowana ścieżka dla zwykłych użytkowników.

## Model topiców

Przykłady używają nowego automatycznego modelu MQTT.

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
- `*_commented.yaml` — ta sama idea z komentarzami.
