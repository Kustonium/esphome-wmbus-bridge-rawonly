# wmbus_radio — minimalna referencja konfiguracji

[English version](CONFIG_REFERENCE_MINIMAL.md)

## Podstawowe

| Opcja | Domyślnie | Status | Opis |
|---|---:|---|---|
| `radio_type` | wymagane | publiczna | `SX1262`, `SX1276`, `CC1101`, `LR1121` |
| `topic_name` | `esphome.name` | publiczna | nazwa bazowa topiców: `wmbus/<topic_name>/...`; bez `/`, spacji, `+`, `#` |
| `listen_mode` | `both` | publiczna | `t1`, `c1`, `both` = tylko T1/C1, `s1` = tylko eksperymentalny S1 |
| `frequency` | zależnie od trybu | publiczna | opcjonalne nadpisanie; T1/C1/both domyślnie `868.950 MHz`, S1 domyślnie `868.300 MHz` |
| `diagnostic_mode` | `off` | publiczna | `off`, `low`, `normal`, `debug`, `dev` |
| `highlight_meters` | puste | publiczna | ID liczników do wyróżnienia i statystyk w `normal/debug`; **nie filtruje MQTT** |
| `forward_meters` | puste | publiczna | whitelista ID publikowanych na `wmbus/<topic_name>/telegram`; lista ID albo `true` = użyj `highlight_meters`; puste = wysyłaj wszystko |
| `publish_rssi` | `false` | publiczna | publikuj RSSI ostatniej ramki każdego licznika na `wmbus/<topic_name>/rssi/<meter_id>`; patrz sekcja niżej |
| `receiver_task_stack_size` | `3072` | zaawansowana | stos osobnego taska RX, zakres `2048..16384` |
| `listen_mode_filter_after_parse` | `false` | eksperymentalna | agresywniejsze filtrowanie po parserze; testować po licznikach, nie po samym globalnym drop% |
| `use_noise_floor_threshold` | `false` | eksperymentalna | próg przerywania słabych startów liczony od ZMIERZONEJ podłogi szumu zamiast od średniej udanych odbiorów; pomiar (`noise_floor_dbm`) działa zawsze, ta opcja tylko go używa |
| `noise_floor_margin_db` | `6` | eksperymentalna | ile dB nad podłogą szumu musi być start, żeby próbować (0–30); działa tylko przy `use_noise_floor_threshold: true` |
| `highlight_ansi` | `false` | publiczna | kolorowanie ANSI wyróżnionych liczników w logu |
| `highlight_tag` | `wmbus_user` | publiczna | tag logu dla wyróżnionych liczników |
| `highlight_prefix` | `"★ "` | publiczna | prefiks linii logu wyróżnionego licznika |
| `allow_untested_framework` | `false` | wymaga jawnej zgody | wymagane do zbudowania na frameworku `arduino`; domyślnie kompilacja jest przerywana |
| `mark_as_handled` | `false` | publiczna | opcja wewnątrz `on_frame:`; oznacza ramkę jako obsłużoną |

## Tryby nasłuchu i częstotliwość

| Tryb | Znaczenie | Domyślna częstotliwość | Uwagi |
|---|---|---:|---|
| `t1` | tylko T1 | `868.950 MHz` | standardowy tryb dla wielu liczników |
| `c1` | tylko C1 | `868.950 MHz` | osobny odbiór C1 |
| `both` | tylko T1/C1 | `868.950 MHz` | nie obejmuje S1 |
| `s1` | tylko S1 | `868.300 MHz` | eksperymentalny tryb diagnostyczny/kompatybilności |

`frequency:` opcjonalnie nadpisuje częstotliwość. Bez tej opcji komponent wybiera wartość domyślną dla trybu. Przykład nadpisania dla testów S1:

```yaml
wmbus_radio:
  listen_mode: s1
  frequency: 868.36
```

Poprawny telegram S1 jest publikowany na `wmbus/<topic_name>/telegram` tak samo jak poprawne telegramy T1/C1. To nie oznacza dekodowania wartości licznika na ESP; tym nadal zajmuje się backend, np. `wmbusmeters`.

## Opcje zależne od radia

| Opcja | Radio | Domyślnie | Status | Opis |
|---|---|---:|---|---|
| `has_tcxo` | `SX1262` | `false` | publiczna | włącz dla płytek SX1262 z TCXO; brak może dawać objaw „Radio active, ale brak ramek” |
| `dio2_rf_switch` | `SX1262` | `true` | publiczna | sterowanie przełącznikiem RF przez DIO2 |
| `rx_gain` | `SX1262` | `boosted` | publiczna | `boosted` albo `power_saving` |
| `long_gfsk_packets` | `SX1262` | `false` | publiczna | zalecane dla długich ramek T1; brak może powodować ucinanie/dropy |
| `sx1262_rx_bandwidth` | `SX1262` | `312khz` | publiczna | `312khz` (domyślne, odziedziczone, niezmierzone dla T1), `234khz`, `156khz`. Działa dla `listen_mode: t1` **i `both`**; `c1` oraz `s1` ignorują — ich 234,3 kHz jest zmierzone i przypięte |
| `fem_ctrl_pin`, `fem_en_pin`, `fem_pa_pin` | `SX1262` | brak | zależna od płytki | piny zewnętrznego front-endu RF, np. Heltec V4 |
| `rf_sw_pin` | `SX1262` | brak | zależna od płytki | bramka wewnętrznego przełącznika RF modułu; wymagane na XIAO ESP32-S3 + Wio-SX1262 (`GPIO38`), inaczej czułość niższa o ~30 dB |
| `sx1276_busy_ether_mode` | `SX1276` | `normal` | publiczna | `normal`, `aggressive`, `adaptive`; podnosić dopiero przy **zmierzonym** przeciążeniu (`fifo_overrun`/`truncated` > 0) — `adaptive` przerywa słabe starty i kosztuje ok. 12 dB czułości |
| `tcxo_pin` | `SX1276` | brak | zależna od płytki | opcjonalny pin TCXO enable; ustawiany HIGH przed inicjalizacją SX1276 |
| `busy_pin` | `SX1262`, `LR1121` | wymagane | publiczna | linia BUSY; bez niej sterownik nie odróżni „jeszcze nie gotowy" od „odpowiedział" |
| `rf_switch` | `SX1262` | brak | zależna od płytki | wymuszenie stanu przełącznika RF; ustawiaj tylko gdy dokumentacja płytki tego wymaga |
| `clear_device_errors_on_boot` | `SX1262` | `false` | zaawansowana | kasuj rejestr błędów układu przy starcie; **zalecane na płytkach z TCXO** — bez tego `XOSC_START_ERR` jest tam zapalone zawsze i nic nie znaczy |
| `publish_dev_err_after_clear` | `SX1262` | `false` | zaawansowana | opublikuj ponownie odczytany stan błędów po skasowaniu; jedyny sposób, by zobaczyć go na węźle, który nic nie odbiera |
| `cc1101_allow_experimental` | `CC1101` | `false` | wymaga jawnej zgody | wymagane do uruchomienia CC1101 |
| `gdo0_pin`, `gdo2_pin` | `CC1101` | wymagane | publiczna | dual IRQ; single-IRQ CC1101 nie jest wspierany |
| `spi_data_rate` | wszystkie | `2000000` (2 MHz) | zaawansowana | zegar SPI **tego urządzenia**, nie całej magistrali. Obniż, zanim zaczniesz podejrzewać układ: moduł na przewodach dupont potrafi gubić bity przy 2 MHz nawet na zdrowych 3,3 V, a objaw jest cichy — rejestry odczytują się jako wartości domyślne i radio zachowuje się jak źle skonfigurowane. Sprawdź `reg_write_retries` w linii `CC1101 debug status`: wartość > 0 potwierdza nieudaną weryfikację zapisu |
| `lr1121_allow_experimental` | `LR1121` | `false` | wymaga jawnej zgody | wymagane do uruchomienia LR1121 |
| `tcxo_voltage` | `LR1121`, `SX1262` | `3.0v` | publiczna | napięcie TCXO modułu; DIO3 w SX1262 to wyjście regulowane z chipu, więc złe napięcie to realne ryzyko dla TCXO, nie kosmetyka |
| `tcxo_startup_ticks` | `LR1121` | `3000` | zaawansowana | czas rozruchu TCXO w taktach 32,768 kHz (~91,6 ms) |
| `rx_bandwidth` | `LR1121` | `234300` | zaawansowana | szerokość pasma RX w Hz |
| `min_preamble_bits` | `SX1262`, `SX1276`, `LR1121` | `16` | zaawansowana | ile bitów preambuły radio musi zobaczyć, zanim zacznie odbiór. **Dla `listen_mode: t1` i `both` maksimum to 16** — preambuła T1 jest krótsza niż 24 bity, więc `24` i `32` dają zero odebranych ramek (zmierzone: 184 wyzwolenia, 0 ramek) i są odrzucane przy walidacji. `8` działa, ale kosztuje ok. 16% słyszanych liczników. Na `SX1276` nie ma wartości `32`. Nie mylić z długością preambuły nadawanej — to osobne pole `SetPacketParams`, nieustawiane z YAML-a |
| `payload_length` | `LR1121` | `255` | zaawansowana | długość stałego przechwycenia T1; host przycina telegram według zdekodowanego L-field |
| `rx_boosted` | `LR1121` | `true` | zaawansowana | +2 dB czułości kosztem ok. 2 mA |
| `bitrate` | `LR1121` | `100000` | zaawansowana | bitrate GFSK |
| `deviation` | `LR1121` | `50000` | zaawansowana | dewiacja GFSK |

`tcxo_pin` dotyczy tylko SX1276. Dla SX1262 używaj `has_tcxo`.

`rf_sw_pin` to nie to samo co `dio2_rf_switch`. DIO2 wybiera kierunek TX/RX wewnątrz układu; `rf_sw_pin` otwiera bramkę przełącznika RF w module i decyduje, czy tor antenowy w ogóle przewodzi. Na płytkach, które tego wymagają, potrzebne są obie opcje.

`wmbus_radio` nie zgaduje okablowania płytki. Opcje takie jak TCXO, RF switch i FEM muszą wynikać ze schematu płytki albo dokumentacji producenta.

## Topiki MQTT

Preferuj `topic_name`.

| Topik | Skąd się bierze | Uwagi |
|---|---|---|
| `wmbus/<topic_name>/telegram` | każda poprawna ramka (lub tylko `forward_meters`) | główne wyjście dla bridge/wmbusmeters |
| `wmbus/<topic_name>/rx` | ta sama poprawna ramka co na `telegram` | strukturalne metadane odbioru; domyślnie QoS 1, bez retain |
| `wmbus/<topic_name>/diag` | drop/rx_path eventy + kopia boot event | główny temat diagnostyczny, bez retain |
| `wmbus/<topic_name>/diag/summary` | co `diagnostic_summary_interval` | globalne summary |
| `wmbus/<topic_name>/diag/summary_15min` | co 15 min | `normal`+ |
| `wmbus/<topic_name>/diag/summary_60min` | co 60 min | tylko `dev`, chyba że wymusisz stare flagi |
| `wmbus/<topic_name>/diag/meter_snapshot` | snapshot liczników | `normal`+ z `highlight_meters`; w `dev` wszystkie |
| `wmbus/<topic_name>/diag/boot` | raz po starcie | `retain=true`; boot idzie też jako kopia do root `diag` bez retain |
| `wmbus/<topic_name>/diag/config` | raz po starcie | `retain=true`; snapshot efektywnej konfiguracji (`{radio, lines[]}`) - konsumowany przez panel diagnostyczny dodatku |
| `wmbus/<topic_name>/diag/suggestion` | wykryta anomalia RF | sugestie diagnostyczne |
| `wmbus/<topic_name>/diag/busy_ether_changed` | zmiana stanu busy-ether | SX1276 + `adaptive` |
| `wmbus/<topic_name>/rssi/<meter_id>` | ramka z realnym pomiarem RSSI | tylko przy `publish_rssi: true`; `retain=true` |

### Strukturalne metadane RX

Każdemu telegramowi dopuszczonemu przez `forward_meters` towarzyszy komunikat
JSON na `wmbus/<topic_name>/rx`. Nie zastępuje on HEX-a na `telegram` i nie
zawiera zdekodowanej wartości licznika. ESP nadal wyłącznie odbiera i
kwalifikuje ramkę RF; zapis do bazy pozostaje zadaniem backendu.

Payload schematu 1 zawiera:

- `boot_id` — identyfikator bieżącego uruchomienia ESP;
- `seq` — wspólny dla źródła, rosnący numer poprawnej ramki;
- `rx_task_wakeup_us` — czas `esp_timer` od uruchomienia, pobrany gdy task RX
  obudził się po IRQ; nie jest to znacznik początku transmisji ani dokładnego
  zdarzenia `RX_DONE`;
- `meter_id`, `mode` (`T1`, `C1` albo `S1`) i `rssi_dbm` (`null`, gdy sterownik
  nie dostarczył rzeczywistego pomiaru);
- `received_at` — czas ODBIORU ramki w ISO-8601 UTC z milisekundami; pole
  nieobecne, dopóki zegar płytki nie jest ustawiony (po restarcie, zanim
  odpowie SNTP)
- `frame_crc32` — IEEE CRC32 końcowych, znormalizowanych bajtów ramki, które są
  publikowane jako HEX;
- `frame_length` — liczba tych bajtów.

`/rx` jest domyślnie publikowany z QoS 1 (`rx_qos`) i `retain=false`. `seq` zwiększa się także dla
poprawnych ramek odebranych podczas braku połączenia MQTT, więc następny
opublikowany komunikat może ujawnić lukę. Luka wskazuje brak zdarzeń na
ścieżce ESP→broker→subskrybent, ale sama nie rozstrzyga, na którym odcinku
powstała. Zmiana `boot_id` rozpoczyna nową domenę numeracji.

Stare opcje ręcznego nadpisania:

| Opcja | Status | Uwagi |
|---|---|---|
| `telegram_topic` | przestarzała | ręczny override, preferuj `topic_name` |
| `diagnostic_topic` | przestarzała | ręczny override, preferuj `topic_name` |

## Whitelista przekazywania

`forward_meters` ogranicza to, co trafia na `wmbus/<topic_name>/telegram`. Typowe
zastosowanie: w eterze słychać dziesiątki liczników sąsiadów, a na brokera mają iść
tylko własne.

```yaml
wmbus_radio:
  forward_meters:
    - 44332211
    - 77665544
```

Jeżeli te same liczniki masz już w `highlight_meters`, nie przepisuj ich drugi raz —
`true` bierze listę stamtąd:

```yaml
wmbus_radio:
  highlight_meters:
    - 44332211
    - 77665544
  forward_meters: true
```

- Puste (domyślnie) albo `false` = zachowanie jak wcześniej, publikowane jest wszystko.
- `forward_meters: true` przy pustym `highlight_meters` **nie** wycisza strumienia:
  filtr się nie włącza, a w logu startowym pojawia się ostrzeżenie.
- Wpisuj ID dokładnie tak, jak pokazuje log — to ten sam zapis, którego używa
  `highlight_meters`:
  - `id:44332211` → `- 44332211` (licznik BCD, zapis dziesiętny),
  - `id:417F0666` → `- "0x417F0666"` (licznik nie-BCD, np. Diehl/IZAR).
- **Wpisy szesnastkowe ujmuj w cudzysłów.** Bez niego YAML sam zamieni `0x417F0666` na
  liczbę `1098843750` i wpis trafiłby na listę dziesiętną, gdzie nigdy z niczym nie
  zrówna. Taki przypadek jest wykrywany przy kompilacji i kończy się błędem
  z podpowiedzią, nie cichym pominięciem.
- Rozróżnienie jest jednoznaczne i nie wymaga wiedzy, który licznik jest który: A-field
  spoza BCD zawsze zawiera cyfrę A–F, a ID w BCD nigdy. Wpis czysto cyfrowy znaczy więc
  „dziesiętne", wpis z literami — „surowe".
- Formy `0x` można użyć również dla licznika BCD (`"0x00088888"` = `88888`), bo surowa
  postać istnieje dla każdego licznika.
- Po starcie log pokazuje sparsowane ID i to, czy przyszły z `highlight_meters`; stan
  filtra jest też w `dump_config()` jako `Forward whitelist:`.
- Filtr działa **po** dekodowaniu i sprawdzeniu DLL CRC, więc dopasowuje ID, które
  parser już zweryfikował.
- Diagnostyka liczy dalej **cały** eter: summary i statystyki RSSI powstają przed
  publikacją, więc widoczność sąsiedztwa zostaje. Obcinany jest sam strumień RAW.
- `target_meter_id` ma własny topic i **nie** podlega whiteliście.

## RSSI per licznik

`publish_rssi` (domyślnie `false`) publikuje siłę sygnału **ostatniej ramki danego
licznika**, osobno dla każdej płytki:

```yaml
wmbus_radio:
  publish_rssi: true
```

```text
wmbus/<topic_name>/rssi/<meter_id>    payload: -52
```

- Payload to sama liczba całkowita w dBm, bez JSON-a; `retain=true`.
- Publikowane są wyłącznie ramki z **rzeczywistym pomiarem**. Gdy sterownik nie
  zatrzasnął RSSI dla danej ramki, nie wysyłamy nic — żadnych sentineli w rodzaju
  `0`, `1` czy `-127`, bo odbiorca nie ma jak odróżnić ich od odczytu.
- Wartość pochodzi z pomiaru zrobionego przy odbiorze tej ramki (SX1276 przy
  pierwszym bajcie, SX1262/LR1121 na sync-word, CC1101 przy odczycie) — nie jest
  to średnia z okna ani pomiar szumu po odbiorze.
- Whitelista `forward_meters` obowiązuje tak samo jak dla telegramów: co nie idzie
  na `telegram`, nie ma też publikowanego RSSI.
- Po stronie dodatku **wMBus MQTT Bridge** każda płytka daje własną encję
  `signal_strength` dla tego samego licznika, więc dwa odbiorniki da się porównać.

**Czego RSSI nie mówi.** Opisuje wyłącznie ramki, które **dotarły i się
zdekodowały**. Licznik na granicy zasięgu, z którego przechodzi co dziesiąta
ramka, pokaże „lepsze" RSSI niż stabilny sąsiad, bo do statystyki trafiają tylko
jego najlepsze próby. Do pytania „czy ten licznik dochodzi" właściwym wskaźnikiem
jest statystyka odbioru 15/60 min, nie RSSI.

## Opcje zaawansowane i deweloperskie

| Opcja | Domyślnie | Status | Opis |
|---|---:|---|---|
| `target_meter_id` | `""` | zaawansowana | osobne przekierowanie jednego licznika |
| `target_topic` | `""` | zaawansowana | topic dla `target_meter_id` |
| `target_log` | `true` | zaawansowana | logowanie trafień target meter |
| `publish_radio_raw` | `false` | deweloperska | surowy tap radiowy na stałym topicu `wmbus_bridge/raw`; nie mylić z normalnym telegramem |
| `diagnostic_publish_suggestion` | z presetu `diagnostic_mode` | zaawansowana | publikuj zdarzenia `suggestion` (podpowiedzi diagnostyczne), dławione do jednej na godzinę na kod; jawne `true`/`false` nadpisuje preset |

## Stare aliasy

`medium` → `normal`

`full` / `raw` → `dev`

Stare flagi `diagnostic_publish_*` zostają tylko dla kompatybilności i wyjątkowych testów.
