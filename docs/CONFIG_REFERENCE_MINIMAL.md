# wmbus_radio minimal configuration reference / minimalna referencja konfiguracji

## Core / podstawowe

| Opcja | Domyślnie | Status | Opis PL / EN |
|---|---:|---|---|
| `radio_type` | wymagane | public | `SX1262`, `SX1276`, `CC1101` |
| `topic_name` | `esphome.name` | public | nazwa bazowa topiców: `wmbus/<topic_name>/...`; bez `/`, spacji, `+`, `#` |
| `listen_mode` | `both` | public | `t1`, `c1`, `both` |
| `diagnostic_mode` | `off` | public | `off`, `low`, `normal`, `debug`, `dev` |
| `highlight_meters` | puste | public | ID liczników do wyróżnienia i statystyk w `normal/debug` |
| `receiver_task_stack_size` | `3072` | advanced | stos osobnego taska RX, zakres `2048..16384` |
| `listen_mode_filter_after_parse` | `false` | experimental | agresywniejsze filtrowanie po parserze; testować po licznikach, nie po samym globalnym drop% |

## MQTT topics / topiki MQTT

Preferuj `topic_name`.

| Topik | Skąd się bierze | Uwagi |
|---|---|---|
| `wmbus/<topic_name>/telegram` | każda poprawna ramka | główny output dla bridge/wmbusmeters |
| `wmbus/<topic_name>/diag` | drop/rx_path eventy + kopia boot event | root diag, bez retain |
| `wmbus/<topic_name>/diag/summary` | co `diagnostic_summary_interval` | globalne summary |
| `wmbus/<topic_name>/diag/summary_15min` | co 15 min | `normal`+ |
| `wmbus/<topic_name>/diag/summary_60min` | co 60 min | tylko `dev`, chyba że wymusisz stare flagi |
| `wmbus/<topic_name>/diag/meter_snapshot` | snapshot liczników | `normal`+ z `highlight_meters`; w `dev` wszystkie |
| `wmbus/<topic_name>/diag/boot` | raz po starcie | `retain=true`; boot idzie też jako kopia do root `diag` bez retain |
| `wmbus/<topic_name>/diag/suggestion` | wykryta anomalia RF | sugestie diagnostyczne |
| `wmbus/<topic_name>/diag/busy_ether_changed` | zmiana stanu busy-ether | SX1276 + `adaptive` |

Legacy/manual override:

| Opcja | Status | Uwagi |
|---|---|---|
| `telegram_topic` | legacy | ręczny override, preferuj `topic_name` |
| `diagnostic_topic` | legacy | ręczny override, preferuj `topic_name` |

## Advanced/dev-only

| Opcja | Domyślnie | Status | Opis |
|---|---:|---|---|
| `target_meter_id` | `""` | advanced | osobne przekierowanie jednego licznika |
| `target_topic` | `""` | advanced | topic dla `target_meter_id` |
| `target_log` | `true` | advanced | logowanie trafień target meter |
| `publish_radio_raw` | `false` | dev-only | surowy tap radiowy na stałym topicu `wmbus_bridge/raw`; nie mylić z normalnym telegramem |

## Deprecated diagnostic aliases / stare aliasy

`medium` → `normal`

`full` / `raw` → `dev`

Stare flagi `diagnostic_publish_*` zostają tylko dla kompatybilności i wyjątkowych testów.
