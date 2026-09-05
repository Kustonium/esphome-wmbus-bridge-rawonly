# Uwagi dotyczące migracji

[English version](MIGRATION.md)

Jeśli wcześniej używałeś jednej ze starych list komponentów zewnętrznych:

```yaml
components: [wmbus_common, wmbus_radio]
```

albo:

```yaml
components: [wmbus_bridge_common, wmbus_radio]
```

zmień ją na:

```yaml
components: [wmbus_radio]
```

Obecny stan repozytorium:

- jeden publiczny komponent ESPHome: `wmbus_radio`,
- wbudowana publikacja RAW przez MQTT z użyciem `telegram_topic`,
- `on_frame` zachowane jako opcjonalne wywołanie dla zaawansowanych zastosowań,
- diagnostyka obejmuje również temat zmian stanu adaptacyjnego mechanizmu SX1276
  (`wmbus/<topic_name>/diag/busy_ether_changed`) oraz temat podpowiedzi
  `wmbus/<topic_name>/diag/suggestion`. Stare formy
  `diagnostic_topic/busy_ether_changed` i `diagnostic_topic/suggestion` pozostają
  wyłącznie jako przestarzałe aliasy dla zgodności ze starszymi konfiguracjami.

Odpowiada to obecnej strukturze repozytorium i pozwala uniknąć starych nazw
zachowanych w pamięci podręcznej wcześniejszych kompilacji.
