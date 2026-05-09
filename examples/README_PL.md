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
