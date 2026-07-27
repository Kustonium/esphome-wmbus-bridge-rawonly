# wmbus_radio minimal configuration reference / minimalna referencja konfiguracji

## Core / podstawowe

| Opcja | Domyślnie | Status | Opis PL / EN |
|---|---:|---|---|
| `radio_type` | wymagane | public | `SX1262`, `SX1276`, `CC1101` |
| `topic_name` | `esphome.name` | public | nazwa bazowa topiców: `wmbus/<topic_name>/...`; bez `/`, spacji, `+`, `#` |
| `listen_mode` | `both` | public | `t1`, `c1`, `both` = T1/C1 only, `s1` = experimental S1 only |
| `frequency` | mode default | public | optional override; T1/C1/both default `868.950 MHz`, S1 default `868.300 MHz` |
| `diagnostic_mode` | `off` | public | `off`, `low`, `normal`, `debug`, `dev` |
| `highlight_meters` | puste | public | ID liczników do wyróżnienia i statystyk w `normal/debug`; **nie filtruje MQTT** |
| `forward_meters` | puste | public | whitelista ID publikowanych na `wmbus/<topic_name>/telegram`; lista ID albo `true` = użyj `highlight_meters`; puste = wysyłaj wszystko |
| `receiver_task_stack_size` | `3072` | advanced | stos osobnego taska RX, zakres `2048..16384` |
| `listen_mode_filter_after_parse` | `false` | experimental | agresywniejsze filtrowanie po parserze; testować po licznikach, nie po samym globalnym drop% |

## Listen modes and frequency / tryby nasłuchu i częstotliwość

| Tryb | Znaczenie | Domyślna częstotliwość | Uwagi |
|---|---|---:|---|
| `t1` | T1 only / tylko T1 | `868.950 MHz` | standardowy tryb dla wielu liczników |
| `c1` | C1 only / tylko C1 | `868.950 MHz` | osobny odbiór C1 |
| `both` | T1/C1 only / tylko T1/C1 | `868.950 MHz` | nie obejmuje S1 |
| `s1` | S1 only / tylko S1 | `868.300 MHz` | eksperymentalny tryb diagnostyczny/kompatybilności |

`frequency:` jest opcjonalnym override. Jeśli go nie podasz, komponent wybiera default na podstawie trybu. Przykład override dla testów S1:

```yaml
wmbus_radio:
  listen_mode: s1
  frequency: 868.36
```

Poprawny telegram S1 jest publikowany na `wmbus/<topic_name>/telegram` tak samo jak poprawne telegramy T1/C1. To nie oznacza dekodowania wartości licznika na ESP; tym nadal zajmuje się backend, np. `wmbusmeters`.

## Radio-specific options / opcje zależne od radia

| Opcja | Radio | Domyślnie | Status | Opis |
|---|---|---:|---|---|
| `has_tcxo` | `SX1262` | `false` | public | włącz dla płytek SX1262 z TCXO; brak może dawać objaw „Radio active, ale brak ramek” |
| `dio2_rf_switch` | `SX1262` | `true` | public | sterowanie przełącznikiem RF przez DIO2 |
| `rx_gain` | `SX1262` | `boosted` | public | `boosted` albo `power_saving` |
| `long_gfsk_packets` | `SX1262` | `false` | public | zalecane dla długich ramek T1; brak może powodować ucinanie/dropy |
| `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin` | `SX1262` | brak | board-specific | piny zewnętrznego front-endu RF, np. Heltec V4 |
| `rf_sw_pin` | `SX1262` | brak | board-specific | bramka wewnętrznego przełącznika RF modułu; wymagane na XIAO ESP32-S3 + Wio-SX1262 (`GPIO38`), inaczej czułość niższa o ~30 dB |
| `sx1276_busy_ether_mode` | `SX1276` | `adaptive` | public | `normal`, `aggressive`, `adaptive` |
| `tcxo_pin` | `SX1276` | brak | board-specific | opcjonalny pin TCXO enable; ustawiany HIGH przed inicjalizacją SX1276 |
| `cc1101_allow_experimental` | `CC1101` | `false` | safety gate | wymagane do uruchomienia CC1101 |
| `gdo0_pin`, `gdo2_pin` | `CC1101` | wymagane | public | dual IRQ; single-IRQ CC1101 nie jest wspierany |

`tcxo_pin` dotyczy tylko SX1276. Dla SX1262 używaj `has_tcxo`.

`rf_sw_pin` to nie to samo co `dio2_rf_switch`. DIO2 wybiera kierunek TX/RX wewnątrz układu; `rf_sw_pin` otwiera bramkę przełącznika RF w module i decyduje, czy tor antenowy w ogóle przewodzi. Na płytkach, które tego wymagają, potrzebne są obie opcje.

`wmbus_radio` nie zgaduje okablowania płytki. Opcje takie jak TCXO, RF switch i FEM muszą wynikać ze schematu płytki albo dokumentacji producenta.

## MQTT topics / topiki MQTT

Preferuj `topic_name`.

| Topik | Skąd się bierze | Uwagi |
|---|---|---|
| `wmbus/<topic_name>/telegram` | każda poprawna ramka (lub tylko `forward_meters`) | główny output dla bridge/wmbusmeters |
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

## Forwarding whitelist / whitelista przekazywania

`forward_meters` ogranicza to, co trafia na `wmbus/<topic_name>/telegram`. Typowe
zastosowanie: w eterze słychać dziesiątki liczników sąsiadów, a na brokera mają iść
tylko własne.

```yaml
wmbus_radio:
  forward_meters:
    - 41551279
    - 90830781
```

Jeżeli te same liczniki masz już w `highlight_meters`, nie przepisuj ich drugi raz —
`true` bierze listę stamtąd:

```yaml
wmbus_radio:
  highlight_meters:
    - 41551279
    - 90830781
  forward_meters: true
```

- Puste (domyślnie) albo `false` = zachowanie jak wcześniej, publikowane jest wszystko.
- `forward_meters: true` przy pustym `highlight_meters` **nie** wycisza strumienia:
  filtr się nie włącza, a w logu startowym pojawia się ostrzeżenie.
- Wpisuj ID dokładnie tak, jak pokazuje log — to ten sam zapis, którego używa
  `highlight_meters`:
  - `id:41551279` → `- 41551279` (licznik BCD, zapis dziesiętny),
  - `id:417F0666` → `- "0x417F0666"` (licznik nie-BCD, np. Diehl/IZAR).
- **Wpisy szesnastkowe ujmuj w cudzysłów.** Bez niego YAML sam zamieni `0x417F0666` na
  liczbę `1098843750` i wpis trafiłby na listę dziesiętną, gdzie nigdy z niczym nie
  zrówna. Taki przypadek jest wykrywany przy kompilacji i kończy się błędem
  z podpowiedzią, nie cichym pominięciem.
- Rozróżnienie jest jednoznaczne i nie wymaga wiedzy, który licznik jest który: A-field
  spoza BCD zawsze zawiera cyfrę A–F, a ID w BCD nigdy. Wpis czysto cyfrowy znaczy więc
  „dziesiętne", wpis z literami — „surowe".
- Formy `0x` można użyć również dla licznika BCD (`"0x00089907"` = `89907`), bo surowa
  postać istnieje dla każdego licznika.
- Po starcie log pokazuje sparsowane ID i to, czy przyszły z `highlight_meters`; stan
  filtra jest też w `dump_config()` jako `Forward whitelist:`.
- Filtr działa **po** dekodowaniu i sprawdzeniu DLL CRC, więc dopasowuje ID, które
  parser już zweryfikował.
- Diagnostyka liczy dalej **cały** eter: summary i statystyki RSSI powstają przed
  publikacją, więc widoczność sąsiedztwa zostaje. Obcinany jest sam strumień RAW.
- `target_meter_id` ma własny topic i **nie** podlega whiteliście.

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
