# esphome-wmbus-bridge (subset)

To jest *subset* repo Szczepana: tylko `wmbus_common` + `wmbus_radio`.
Nie ma `wmbus_meter` (dekodowania) – ESP wysyła telegramy, dekoder jest poza ESP.

Komponent wykrywa ramki T2 i tylko je wykrywa sygnalizując obecność takich ramek

## WYMAGANIA ##
Płytka ESP z radiem SX1276 

## Użycie

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge@main
    components: [wmbus_common, wmbus_radio]
    refresh: 0d
```

## Filtr ramek (ważne)

- `frame->as_hex().size() >= 30` – wygodne, ale generuje HEX nawet dla śmieci.
- `frame->size() >= 15` – to samo logicznie (30 hex = 15 bajtów), ale taniej na słabszych ESP.

## Atrybucja i licencja
- SzczepanLeon/esphome-components
- wmbusmeters/wmbusmeters


GPL-3.0-or-later – patrz `LICENSE` i `NOTICE`.
