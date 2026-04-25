# DIAGNOSTIC_PL.md

[English version](DIAGNOSTIC.md)

Dokładny opis diagnostyki, pól MQTT i opcji YAML dla `wmbus_radio`.

Zobacz też:

- [`CHIP_SELECTION_PL.md`](CHIP_SELECTION_PL.md)
- [`BENCHMARKS_PL.md`](BENCHMARKS_PL.md)
- [`TROUBLESHOOTING_PL.md`](TROUBLESHOOTING_PL.md)

---

## 1. Ważna uwaga o języku komunikatów

Dokumentacja jest rozdzielona na osobne wersje polską i angielską.

Logowanie runtime przyjmuje praktyczną zasadę:

- najważniejsze komunikaty użytkowe `INFO` / `WARN` / `ERROR` mogą być krótkie i dwujęzyczne `EN / PL`,
- niskopoziomowe komunikaty `DEBUG` / `VERBOSE` pozostają po angielsku,
- nazwy opcji YAML, eventów MQTT i pól JSON pozostają po angielsku jako stabilne API techniczne.

Za stabilne API tego projektu należy traktować przede wszystkim:

- nazwy opcji YAML,
- nazwy pól JSON publikowanych do MQTT,
- semantykę eventów (`boot`, `summary`, `dropped`, `truncated`, `rx_path`, `meter_window`, `busy_ether_changed`, `suggestion`, `dev_err_cleared`).

Komentarze w przykładach i dokumentacji opisują, jak te pola czytać.

Większość eventów diagnostycznych MQTT zawiera teraz także `uptime_ms` i `listen_mode`, żeby łatwiej było porównywać dłuższe okna bez zgadywania, w jakim trybie pracowało radio.

---

## 2. Co jest „telegramem” w tym repo

W tym repo występują dwa różne typy danych.

### A. RAW telegram do dalszego dekodowania

To jest zwykły payload HEX publikowany np. na:

- `wmbus_bridge/telegram`

To **nie jest JSON** i nie ma opisowych pól.

Najczęściej używasz:

- `telegram_topic` — preferowana wbudowana publikacja zweryfikowanego HEX telegramu po MQTT
- `frame->as_hex()` — HEX po usunięciu CRC DLL, najlepszy do `wmbusmeters`, gdy potrzebujesz własnej logiki w `on_frame`
- `frame->as_rtlwmbus()` — tekst w stylu rtl-wmbus do opcjonalnych dodatkowych topiców

### B. Telegram diagnostyczny

To jest JSON publikowany do `diagnostic_topic`.

Eventy:

- `boot`
- `summary`
- `dropped`
- `truncated`
- `rx_path`
- `meter_window`
- `busy_ether_changed`
- `suggestion`
- `dev_err_cleared`

---

## 3. Wszystkie opcje YAML

### 3.1 Wymagane

| Klucz | Opis |
|---|---|
| `radio_type` | `SX1262` albo `SX1276` |
| `reset_pin` | Pin RESET radia |
| `irq_pin` | Pin IRQ / DIO radia używany przez driver |
| `spi_id`, `clk_pin`, `mosi_pin`, `miso_pin`, `cs_pin` | Konfiguracja SPI |

### 3.2 Ogólne

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `busy_pin` | brak | Pin BUSY dla SX1262. Dla SX1276 nie używać. |
| `listen_mode` | `both` | `t1`, `c1`, `both`. Filtruje, co komponent liczy i przekazuje dalej. |
| `on_frame` | brak | Callback dla każdej poprawnej ramki. |

### Jak działa `listen_mode`

- `t1` — tylko T1
- `c1` — tylko C1
- `both` — T1 + C1

Filtr działa **przed** dalszą obróbką i **przed licznikami diagnostycznymi**.

To ważne:

- `summary.total` nie liczy ramek odfiltrowanych przez `listen_mode` ani kandydatów odrzuconych przed kolejką, np. słabych partial-startów albo false-startów bez wyliczonej długości
- `dropped_by_stage.link_mode` nie oznacza „nie tego trybu z YAML”, tylko problemy z rozpoznaniem trybu

### Kiedy używać którego trybu

- **Polska, większość wodomierzy i ciepłomierzy T1** → zacznij od `t1`
- **Masz pewność, że licznik nadaje C1** → użyj `c1`
- **Mieszane środowisko i testy** → użyj `both`

Uwaga: na **SX1276** tryb `both` ma realny koszt odbioru.

---

### 3.3 Bezpośrednia publikacja RAW

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `telegram_topic` | brak | Główny topic dla zweryfikowanego HEX telegramu (`frame->as_hex()`); nieudane kandydaty nie są forwardowane. |
| `target_meter_id` | brak | Wybrane ID licznika do osobnego topicu. |
| `target_topic` | brak | Osobny topic dla `target_meter_id`. |
| `target_log` | `true` | Czy logować pakiety wybranego licznika. |

---

### 3.4 SX1262 – opcje specyficzne

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `dio2_rf_switch` | `true` | Użyj DIO2 jako przełącznika RF. |
| `rf_switch` | brak | Alias dla `dio2_rf_switch`. |
| `has_tcxo` | `false` | Włącz TCXO zamiast zwykłego kwarcu. |
| `rx_gain` | `boosted` | `boosted` albo `power_saving`. |
| `long_gfsk_packets` | `false` | Dłuższe pakiety GFSK dla trudniejszych ramek. |
| `fem_ctrl_pin` | brak | Pin sterujący torem RX/TX FEM. |
| `fem_en_pin` | brak | Pin enable FEM / LNA. |
| `fem_pa_pin` | brak | Pin enable PA. |
| `clear_device_errors_on_boot` | `false` | Jednorazowo czyść latched device errors na starcie. |
| `publish_dev_err_after_clear` | `false` | Publikuj `dev_err` przed i po clear. Wymaga `diagnostic_topic`. |

### Kiedy włączyć `long_gfsk_packets`

Włącz to, gdy masz dłuższe telegramy albo trudny eter i zwykły tryb nie daje stabilnego RX.

---

### 3.5 SX1276 – opcje specyficzne

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `sx1276_busy_ether_mode` | `adaptive` | `normal`, `aggressive`, `adaptive` |

### Co robi `sx1276_busy_ether_mode`

- `normal`  
  Brak dodatkowego filtrowania. Radio próbuje czytać wszystko.

- `adaptive`  
  Tryb domyślny. Komponent ocenia bieżące krótkie okno diagnostyczne i włącza ostrzejszą filtrację tylko wtedy, gdy okno wygląda na realny przypadek busy ether.

- `aggressive`  
  Ostrzejsza filtracja jest cały czas włączona.

### Jak naprawdę działa `adaptive`

`adaptive` nie jest ogólnym auto-presetem, tylko konkretnym algorytmem dla **SX1276**, ocenianym **raz na krótkie okno `summary`, przed wyzerowaniem liczników**.

Buduje lokalny wskaźnik false-start-like z:

- `preamble_read_failed`
- `payload_size_unknown`
- `weak_start_aborted`
- `probe_start_aborted`
- `raw_drain_skipped_weak`

Następnie sprawdza, czy bieżące okno spełnia któryś z warunków wyzwolenia:

- dowolny `fifo_overrun` (zawsze krytyczny — przepełnienie hardware)
- `false_start_like >= 80` **i** `drop_pct >= 10`
- `preamble_read_failed >= 25` **i** `probe_start_aborted >= 20` **i** `drop_pct >= 10`
- `drop_pct >= 20` **i** `false_start_like >= 30`
- `t1_sym_invalid_pct >= 5` **i** `false_start_like >= 20` **i** `drop_pct >= 10`

Jeżeli któryś warunek zajdzie, `adaptive` wchodzi w **5-minutowy hold**, w którym SX1276 używa ostrzejszych decyzji dla weak-start / probe-start / raw-drain. Jeżeli kolejne okno nadal jest zaszumione, ten 5-minutowy hold zostaje przedłużony.

Przy zmianie stanu repo publikuje:

- `diagnostic_topic/busy_ether_changed` z `state=adaptive_active`
- `diagnostic_topic/busy_ether_changed` z `state=adaptive_passive`

Dlatego `summary` może wyglądać czyściej na SX1276: część złych startów jest odrzucana jeszcze zanim stanie się pełną próbą decode.

### Praktyczna sugestia

**Zostaw `adaptive`, jeśli:**

- mieszkasz w bloku albo innym gęstym środowisku RF,
- jeszcze nie wiesz, jak czysty jest eter,
- widzisz dużo `false_start_like`, `probe_start_aborted`, `preamble_read_failed`,
- `meter_window` pokazuje realne straty mimo pozornie czystego `summary`.

**Przełącz na `normal`, jeśli:**

- masz mało liczników,
- interwały są długie,
- eter jest spokojny,
- `summary` i `meter_window` wyglądają dobrze bez oznak zatłoczenia.

**`aggressive` używaj tylko świadomie**, zwykle do testów albo bardzo ciężkiego eteru.

Bo może przyciąć prawdziwe, ale słabsze liczniki.

---

### 3.6 Diagnostyka

Publikacja diagnostyki jest teraz domyślnie **opt-in**.

`diagnostic_mode` steruje tylko **publikacją diagnostyki MQTT i poziomem gadatliwości**.
Nie wyłącza wewnętrznych liczników, okien czasowych ani logiki radiowej wymaganej przez funkcje takie jak tryb `adaptive` w SX1276.

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `diagnostic_mode` | `off` | Preset diagnostyki MQTT: `off`, `low`, `medium`, `full`. |
| `diagnostic_topic` | auto | Bazowy topic diagnostyki. W trybie `off` domyślnie pusty; dla presetów innych niż `off` domyślnie `wmbus/diag`. Jeżeli jakiekolwiek `diagnostic_publish_*` zostanie jawnie włączone bez topicu, komponent użyje fallbacku `wmbus/diag`. |
| `diagnostic_verbose` | zależne od presetu | Loguj szczegóły dropów i truncate także do serial/API. Jawne opcje YAML nadpisują preset. |
| `diagnostic_publish_summary` | zależne od presetu | Publikuj okresowy `summary`. Jawne opcje YAML nadpisują preset. |
| `diagnostic_publish_drop_events` | zależne od presetu | Publikuj eventy `dropped` / `truncated`. Jawne opcje YAML nadpisują preset. |
| `diagnostic_publish_rx_path_events` | zależne od presetu | Publikuj bieżące eventy `rx_path`. Jawne opcje YAML nadpisują preset. |
| `diagnostic_publish_highlight_only` | zależne od presetu | Ogranicz per-packet diag do ID z `highlight_meters`. Jawne opcje YAML nadpisują preset. |
| `diagnostic_publish_summary_highlight_meters` | zależne od presetu | Po każdym `summary_15min` i `summary_60min` publikuje snapshot `meter_window` dla każdego ID z `highlight_meters`. Tylko do odczytu — nie resetuje liczników per-licznik. Jawne opcje YAML nadpisują preset. |
| `diagnostic_publish_raw` | zależne od presetu | Dołącz `raw(hex)` do eventów `dropped` / `truncated`. Jawne opcje YAML nadpisują preset. |
| `diagnostic_summary_interval` | `60s` | Co ile publikować krótkie okno `summary`, gdy publikacja summary jest włączona. |
| `diagnostic_publish_summary_15min` / `diagnostic_publish_summary_60min` | wyłączone | Opcjonalne dodatkowe stałe okna `summary`: 15 minut i 60 minut, włączane niezależnie przez booleany. Publikowane w tym samym formacie payloadu plus `interval_s`, `uptime_ms` i `listen_mode`. Topiki są osobne: `/summary`, `/summary_15min` i `/summary_60min` pod `diagnostic_topic`. Uwaga: pole `busy_ether_state` jest tylko w krótkim `/summary`, nie ma go w `/summary_15min` ani `/summary_60min`. |

### Presety diagnostyczne

- `off` — brak diagnostyki MQTT; `highlight_meters` wpływa tylko na lokalne wyróżnienie logów
- `low` — lekka diagnostyka, oparta głównie na summary
- `medium` — summary plus najbardziej użyteczne eventy drop/truncate
- `full` — pełna diagnostyka MQTT, w tym raw i RX-path

Szczegółowe opcje `diagnostic_publish_*` nadal istnieją i nadpisują wybrany preset.

### Pochodne topiki diagnostyczne bez osobnych kluczy YAML

Poniższe topiki pojawiają się automatycznie pod `diagnostic_topic`, gdy dana funkcja jest aktywna:

- `/busy_ether_changed` — zmiany stanu adaptive na SX1276 (`adaptive_active` / `adaptive_passive`)
- `/suggestion` — ograniczane częstotliwościowo wskazówki wynikające z bieżącego okna diagnostycznego

### Jak działa topic `suggestion`

`diagnostic_topic/suggestion` jest pomocniczym topicem pochodnym. **Nie jest retainowany** i ma **limit jednego komunikatu na godzinę dla każdego kodu sugestii**.

Aktualne pola payloadu to:

- `event` = `suggestion`
- `chip`
- `code`
- `yaml_key`
- `suggested_value`
- `yaml_snippet` — gotowy fragment YAML do wklejenia bezpośrednio do konfiguracji
- `hint_en`
- `hint_pl`

Bieżąca implementacja może publikować m.in.:

- `NO_METERS_DETECTED` — brak jakichkolwiek ramek
- `ADD_HIGHLIGHT_METERS` — ramki są, ale `highlight_meters` nadal jest puste
- `ENABLE_RX_PATH_EVENTS` — SX1276 pokazuje dużo false startów
- `ENABLE_DROP_EVENTS_RAW` — dużo dropów przy słabym RSSI, więc warto podejrzeć pakiety głębiej
- `SX1262_SYMBOL_ERRORS` — SX1262 pokazuje błędy symboli T1, więc warto ustawić `cpu_frequency: 160MHz`
- `QUIET_ETHER_ADAPTIVE_IDLE` — SX1276 w `adaptive` pozostaje spokojny na tyle długo, że warto rozważyć test `normal`

### Proste profile do codziennego użycia

Cichy tryb produkcyjny po sprawdzeniu liczników:

```yaml
diagnostic_mode: off
```

Lekka diagnostyka na żądanie:

```yaml
diagnostic_topic: "wmbus/my_receiver/diag"
diagnostic_mode: low
```

W razie potrzeby szczegółowe flagi nadal mogą nadpisywać preset.

---

### 3.7 Highlight i statystyki per licznik

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `highlight_meters` | `[]` | Lista ID liczników do wyróżnienia i śledzenia. |
| `highlight_ansi` | `false` | ANSI color w logach. |
| `highlight_tag` | `wmbus_user` | Osobny tag logów dla wyróżnionych liczników. |
| `highlight_prefix` | `★ ` | Prefiks komunikatu w logu. |

Domyślnie `highlight_meters` wpływa tylko na lokalne wyróżnienie logów.

Samo w sobie **nie** włącza diagnostyki MQTT i **nie** oznacza publikacji `meter_window`.

Snapshoty `meter_window` per licznik są publikowane dopiero wtedy, gdy diagnostyka jest włączona i ustawisz `diagnostic_publish_summary_highlight_meters: true`.

Jeżeli licznik wysyła **T1 i C1 pod tym samym ID**, statystyki pozostają rozdzielone.

---

## 4. Metody dostępne w `on_frame`

| Metoda | Zwraca | Znaczenie |
|---|---|---|
| `frame->as_hex()` | `std::string` | HEX bez CRC DLL, zwykle do `wmbusmeters` |
| `frame->as_raw()` | `std::vector<uint8_t>` | Surowe bajty ramki |
| `frame->as_rtlwmbus()` | `std::string` | Tekstowy format rtl-wmbus |
| `frame->rssi()` | `int8_t` | RSSI w dBm |
| `frame->link_mode()` | `LinkMode` | `T1` albo `C1` |
| `frame->format()` | `std::string` | `"A"` albo `"B"` |

Przykład:

```yaml
wmbus_radio:
  on_frame:
    - then:
        - mqtt.publish:
            topic: "wmbus_bridge/my_receiver/telegram"
            payload: !lambda |-
              return frame->as_hex();
```

`telegram_topic` używaj do głównego strumienia zweryfikowanych telegramów. `on_frame` zostaw tylko do dodatków, np. migania LED, osobnego topicu RSSI albo alternatywnego formatu.

---

## 5. Tematy MQTT

### 5.1 RAW telegram

- `telegram_topic` — preferowany dla głównego strumienia zweryfikowanych telegramów
- topic publikowany ręcznie z `on_frame` — tylko gdy potrzebujesz własnej logiki albo dodatkowych formatów

Najczęstszy przykład:

- `wmbus_bridge/telegram`

### 5.2 Diagnostyka

Bazowy topic:

- `diagnostic_topic`

Domyślnie:

- `wmbus/diag`

Topiki `summary` są osobne:

- krótkie summary: `{diagnostic_topic}/summary`
- summary 15-minutowe: `{diagnostic_topic}/summary_15min`
- summary 60-minutowe: `{diagnostic_topic}/summary_60min`

Eventy per-packet takie jak `dropped`, `truncated`, `rx_path` oraz boot/device-error dalej używają bazowego `diagnostic_topic`.

### 5.3 `meter_window`

Dla meter windows topic jest rozszerzony do:

- `{diagnostic_topic}/meter/{meter_id}/{mode}/window/{trigger}`

Przykład:

- `wmbus/diag/meter/00089907/T1/window/count`

---

## 6. Event `boot`

Publikowany raz po starcie.

Przykład:

```json
{"event":"boot","radio":"SX1262","listen_mode":"T1+C1 (both, 3:1 bias)","uptime_ms":8120}
```

| Pole | Znaczenie |
|---|---|
| `event` | zawsze `boot` |
| `radio` | nazwa radia, np. `SX1262`, `SX1276` |
| `listen_mode` | aktywny tryb nasłuchu w formie tekstowej |
| `uptime_ms` | uptime w milisekundach w chwili publikacji |

Jak to czytać:

- służy głównie do potwierdzenia, że urządzenie wstało i w jakim trybie pracuje

---

## 7. Event `summary`

To najważniejszy payload zbiorczy.

Przykład:

```json
{
  "event":"summary",
  "interval_s":60,
  "uptime_ms":812000,
  "listen_mode":"T1 only",
  "total":25,
  "ok":22,
  "truncated":1,
  "dropped":2,
  "crc_failed":1,
  "crc_fail_pct":4,
  "drop_pct":8,
  "trunc_pct":4,
  "avg_ok_rssi":-70,
  "avg_drop_rssi":-88
}
```

### 7.1 Pola główne

| Pole | Znaczenie |
|---|---|
| `event` | zawsze `summary` |
| `interval_s` | rzeczywista długość tego okna w sekundach |
| `uptime_ms` | uptime urządzenia w chwili publikacji |
| `listen_mode` | skonfigurowany tryb odbioru: `T1 only`, `C1 only` albo `T1+C1 (both, 3:1 bias)` |
| `total` | liczba ramek, które przeszły `listen_mode` i weszły do parsera |
| `ok` | ramki poprawnie złożone do końca |
| `truncated` | ramki urwane / niepełne |
| `dropped` | ramki odrzucone jako błędne |
| `crc_failed` | ile dropów to błędy DLL CRC |
| `crc_fail_pct` | procent `crc_failed` względem `total` |
| `drop_pct` | procent `dropped` względem `total` |
| `trunc_pct` | procent `truncated` względem `total` |
| `avg_ok_rssi` | średnie RSSI poprawnych ramek |
| `avg_drop_rssi` | średnie RSSI odrzuconych ramek |

### 7.2 Sekcje `t1` i `c1`

Obie sekcje mają prawie te same pola.

#### `t1`

| Pole | Znaczenie |
|---|---|
| `total` | liczba ramek T1 w oknie |
| `ok` | poprawne T1 |
| `dropped` | odrzucone T1 |
| `per_pct` | procent dropów w obrębie T1 |
| `crc_failed` | T1 odrzucone przez DLL CRC |
| `crc_pct` | procent T1 z błędem CRC |
| `avg_ok_rssi` | średnie RSSI poprawnych T1 |
| `avg_drop_rssi` | średnie RSSI odrzuconych T1 |
| `sym_total` | liczba symboli 3-of-6 policzonych dla T1 |
| `sym_invalid` | liczba błędnych symboli 3-of-6 |
| `sym_invalid_pct` | procent błędnych symboli 3-of-6 |

#### `c1`

| Pole | Znaczenie |
|---|---|
| `total` | liczba ramek C1 w oknie |
| `ok` | poprawne C1 |
| `dropped` | odrzucone C1 |
| `per_pct` | procent dropów w obrębie C1 |
| `crc_failed` | C1 odrzucone przez DLL CRC |
| `crc_pct` | procent C1 z błędem CRC |
| `avg_ok_rssi` | średnie RSSI poprawnych C1 |
| `avg_drop_rssi` | średnie RSSI odrzuconych C1 |

### 7.3 `dropped_by_reason`

| Pole | Znaczenie |
|---|---|
| `too_short` | za mało danych nawet na sensowną próbę |
| `decode_failed` | dekoder nie złożył ramki |
| `dll_crc_failed` | payload wyglądał sensownie, ale DLL CRC nie przeszedł |
| `unknown_preamble` | nie rozpoznano preambuły |
| `l_field_invalid` | L-field nie pasuje do ramki |
| `unknown_link_mode` | nie udało się poprawnie ustalić trybu link mode |
| `other` | reszta przypadków |

### 7.4 `dropped_by_stage`

| Pole | Znaczenie |
|---|---|
| `precheck` | błąd jeszcze przed właściwym parserem |
| `t1_decode3of6` | błąd dekodowania 3-of-6 T1 |
| `t1_l_field` | błąd L-field dla T1 |
| `t1_length_check` | niespójna długość T1 |
| `c1_precheck` | wstępny błąd C1 |
| `c1_preamble` | problem na preambule C1 |
| `c1_suffix` | problem na suffixie C1 |
| `c1_l_field` | błąd L-field dla C1 |
| `c1_length_check` | niespójna długość C1 |
| `dll_crc_first` / `dll_crc_mid` / `dll_crc_final` | miejsce padnięcia DLL CRC dla formatów blokowych |
| `dll_crc_b1` / `dll_crc_b2` | DLL CRC dla bloków B1/B2 |
| `link_mode` | problem z rozpoznaniem trybu link mode |
| `other` | reszta przypadków |

### 7.5 `rx_path`

To sekcja o tym, co działo się **po stronie odbiorczej**, zanim parser miał gotową ramkę.

| Pole | Znaczenie |
|---|---|
| `irq_timeout` | timeout oczekiwania na IRQ |
| `preamble_read_failed` | nie udało się sensownie odczytać preambuły |
| `preamble_retry_recovered` | preambuła najpierw wyglądała źle, ale retry ją uratował |
| `t1_header_read_failed` | nie udało się sensownie odczytać nagłówka T1 |
| `payload_size_unknown` | po początku ramki nie dało się ustalić rozmiaru |
| `raw_drain_attempted` | ile razy driver próbował dozbierać strumień „na ślepo” |
| `raw_drain_recovered` | ile takich prób zakończyło się odzyskaniem ramki |
| `raw_drain_recovery_pct` | skuteczność powyższego mechanizmu |
| `raw_drain_bytes` | ile dodatkowych bajtów w ten sposób doczytano |
| `payload_read_failed` | nie udało się dokończyć odczytu payloadu |
| `queue_send_failed` | kolejka do taska była pełna albo zajęta |
| `fifo_overrun` | przepełnienie FIFO / RX path |
| `weak_start_aborted` | odcięto bardzo słaby początek ramki |
| `probe_start_aborted` | odcięto słaby start już na etapie T1 probe |
| `raw_drain_skipped_weak` | nie próbowano raw-drain, bo start był za słaby |
| `false_start_like` | zbiorczy licznik „śmieciowych” startów |

#### `probe_abort_rssi` i `weak_abort_rssi`

To histogramy RSSI dla abortów.

Buckety:

- `gt70` → mocniejsze niż –70 dBm
- `70_79` → –70 do –79 dBm
- `80_89` → –80 do –89 dBm
- `90_99` → –90 do –99 dBm
- `lt100` → –100 dBm i słabiej

### 7.6 `reasons_sum` i `reasons_sum_mismatch`

| Pole | Znaczenie |
|---|---|
| `reasons_sum` | suma wszystkich liczników z `dropped_by_reason` |
| `reasons_sum_mismatch` | `1` jeśli suma nie zgadza się z `dropped`, inaczej `0` |

To pole jest stricte kontrolne.

### 7.7 `hint_code`, `hint_en`, `hint_pl`

To automatyczna sugestia diagnostyczna.

Przykładowe kody:

- `GOOD`
- `NO_DATA`
- `WEAK_SIGNAL`
- `T1_SYMBOL_ERRORS`
- `T1_BITFLIPS`
- `SX1276_BUSY_ETHER`
- `C1_OVERLOAD_OR_MULTIPATH`

Jak to czytać:

- traktuj to jako **podpowiedź**, nie wyrok
- najpierw patrz na liczby, potem na hint

### Najważniejsza interpretacja `summary`

`summary` mówi, jak czysto idzie **parser i RX path** w danym oknie.

Ale nie mówi całej prawdy o skuteczności dla konkretnego licznika.

Dlatego:

- `summary` = stan ogólny
- `meter_window` = realna skuteczność konkretnego licznika

To szczególnie ważne na SX1276.

---

## 8. Event `dropped`

Publikowany dla odrzuconej ramki.

Przykład:

```json
{
  "event":"dropped",
  "reason":"decode_failed",
  "stage":"t1_decode3of6",
  "detail":"invalid_3of6_symbol",
  "mode":"T1",
  "rssi":-86,
  "want":56,
  "got":48,
  "raw_got":48,
  "decoded_len":48,
  "final_len":48,
  "dll_crc_removed":0,
  "suffix_ignored":0
}
```

Jeśli `diagnostic_publish_raw: true`, dostajesz jeszcze:

- `raw`

| Pole | Znaczenie |
|---|---|
| `event` | zawsze `dropped` |
| `reason` | główny powód odrzucenia |
| `stage` | etap pipeline, na którym ramka poległa |
| `detail` | dokładniejszy opis techniczny |
| `mode` | `T1` albo `C1` |
| `rssi` | RSSI tej próby |
| `want` | ile bajtów parser chciał mieć |
| `got` | ile bajtów parser realnie dostał |
| `raw_got` | ile surowych bajtów dotarło z RX path |
| `decoded_len` | długość po dekodowaniu, np. po 3-of-6 |
| `final_len` | długość końcowa po obróbce |
| `dll_crc_removed` | ile bloków CRC DLL zdjęto |
| `suffix_ignored` | czy suffix został zignorowany |
| `raw` | surowy HEX do analizy ręcznej |

Jak to czytać:

- `reason` mówi **co** padło
- `stage` mówi **gdzie**
- `detail` mówi **jak dokładnie**

---

## 9. Event `truncated`

Prawie to samo co `dropped`, ale oznacza ramkę urwaną / niedoczytaną.

Pola są takie same jak w `dropped`.

Praktycznie:

- dużo `truncated` zwykle oznacza problem z odczytem końcówki, kolizje, presję FIFO, zakłócenia albo słaby sygnał

---

## 10. Event `rx_path`

To pojedynczy event „na żywo” z toru odbiorczego.

Przykład:

```json
{"event":"rx_path","stage":"receive_probe_start","rssi":-94,"detail":"weak_t1_probe_start"}
```

| Pole | Znaczenie |
|---|---|
| `event` | zawsze `rx_path` |
| `stage` | etap RX path |
| `rssi` | RSSI w chwili zdarzenia |
| `detail` | detal techniczny, jeśli istnieje |

To jest przydatne głównie do debugowania drivera i eteru.

Na co dzień zwykle warto to wyłączyć.

---

## 11. Event `meter_window`

To najważniejszy event **per licznik**.

Topic:

- `{diagnostic_topic}/meter/{meter_id}/{mode}/window/{trigger}`

Przykład:

```json
{
  "event":"meter_window",
  "uptime_ms":812000,
  "listen_mode":"T1 only",
  "trigger":"count",
  "id":"00089907",
  "mode":"T1",
  "elapsed_s":312,
  "count_window":10,
  "count_total":42,
  "avg_interval_s":30,
  "win_avg_interval_s":30,
  "win_interval_n":9,
  "last_rssi":-59,
  "win_avg_rssi":-59
}
```

| Pole | Znaczenie |
|---|---|
| `event` | zawsze `meter_window` |
| `uptime_ms` | uptime urządzenia w chwili publikacji |
| `listen_mode` | skonfigurowany tryb odbioru aktywny przy publikacji triggera |
| `trigger` | `count` albo `time` |
| `id` | ID licznika |
| `mode` | `T1` albo `C1` |
| `elapsed_s` | długość bieżącego okna w sekundach |
| `count_window` | ile pakietów przyszło w tym oknie |
| `count_total` | ile pakietów przyszło łącznie od startu |
| `avg_interval_s` | średni interwał liczony po całym życiu licznika |
| `win_avg_interval_s` | średni interwał tylko w tym oknie |
| `win_interval_n` | ile interwałów weszło do średniej okna |
| `last_rssi` | RSSI ostatniego pakietu |
| `win_avg_rssi` | średnie RSSI w tym oknie |

### Dwa triggery

- `count` — po określonej liczbie pakietów
- `time` — po określonym czasie

Po co oba?

- `count` lepiej pokazuje regularność, gdy pakiety wpadają często
- `time` daje heartbeat nawet dla rzadszych liczników

### Dlaczego `meter_window` jest tak ważny

Bo pokazuje **realną skuteczność odbioru konkretnego licznika**.

Jeśli licznik powinien nadawać co 30 s, a `win_avg_interval_s` wynosi 90 s, to gubisz około 2/3 ramek — nawet jeśli `summary` wygląda dobrze.

---

## 12. Event `dev_err_cleared` (SX1262)

Publikowany raz po starcie, jeśli włączysz:

- `clear_device_errors_on_boot: true`
- `publish_dev_err_after_clear: true`

Przykład:

```json
{"event":"dev_err_cleared","before":4,"before_hex":"0004","after":0,"after_hex":"0000"}
```

| Pole | Znaczenie |
|---|---|
| `event` | zawsze `dev_err_cleared` |
| `before` | wartość błędów urządzenia przed czyszczeniem |
| `before_hex` | to samo w HEX |
| `after` | wartość po czyszczeniu |
| `after_hex` | to samo w HEX |

Jak to czytać:

- `before != 0` nie musi oznaczać awarii teraz — może to być latched stan z bootu
- `after = 0` oznacza, że czyszczenie się udało

---

## 13. Jak to czytać w praktyce

### 13.1 Zacznij od rozsądnego profilu

```yaml
listen_mode: t1                # jeśli głównie masz T1
diagnostic_verbose: false
diagnostic_publish_summary: true
diagnostic_publish_drop_events: true
diagnostic_publish_rx_path_events: false
diagnostic_publish_highlight_only: true
diagnostic_publish_raw: false
sx1276_busy_ether_mode: adaptive   # dla SX1276
```

### 13.2 Patrz w tej kolejności

1. `boot`  
   Czy urządzenie wystartowało tak, jak oczekujesz?

2. `summary`  
   Czy RX path wygląda czysto czy brudno?

3. `meter_window`  
   Czy konkretny licznik wpada tak często, jak powinien?

4. `dropped` / `truncated`  
   Jeśli nie, to **gdzie** i **dlaczego** się sypie?

5. `rx_path`  
   Dopiero gdy trzeba debugować głębiej.

### 13.3 Typowe wnioski

#### `summary` dobre, `meter_window` złe

Najczęściej oznacza:

- kolizje,
- busy ether,
- koszt trybu `both`,
- ograniczenia SX1276.

**Dowód praktyczny — zmierzony w tym samym środowisku bloku mieszkalnego, to samo okno 900 s:**

| | SX1262 (Heltec) | SX1276 (Lilygo) |
|---|---|---|
| `summary drop_pct` | 12% | 2% |
| `summary hint` | OK | GOOD |
| `meter_window count` | 28 / 30 | 17 / 30 |
| `meter_window skuteczność` | ~93% | ~57% |
| RSSI | –74 dBm | –48 dBm |

`summary` pokazuje SX1276 jako lepszy. `meter_window` pokazuje coś odwrotnego.
SX1276 ma mocniejszy sygnał i mimo to gubi więcej pakietów — bo `adaptive` ucina problematyczne próby odbioru przed decode, co obniża `drop_pct` bez poprawy realnego odbioru.

#### Wysokie `dll_crc_failed` przy dobrym RSSI

Najczęściej oznacza:

- przester,
- multipath,
- lokalne zakłócenia,
- a nie tylko „słaby sygnał”.

#### Wysokie `preamble_read_failed` / `probe_start_aborted`

Najczęściej oznacza:

- śmieci w eterze,
- dalekie albo nakładające się liczniki,
- SX1276 raczej powinien zostać na `adaptive`.

#### `win_avg_interval_s` dużo większe niż powinno

To najmocniejszy dowód, że pakiety realnie giną.

---

## 14. Przykład konfiguracji pod debug

```yaml
wmbus_radio:
  radio_type: SX1276
  listen_mode: t1
  sx1276_busy_ether_mode: adaptive

  highlight_meters:
    - "00089907"

  diagnostic_topic: "wmbus/lilygo/diag"
  diagnostic_summary_interval: 60s
  diagnostic_publish_summary_15min: true
  diagnostic_publish_summary_60min: false
# summary topics: <diagnostic_topic>/summary, /summary_15min, /summary_60min
  diagnostic_verbose: false
  diagnostic_publish_summary: true
  diagnostic_publish_drop_events: true
  diagnostic_publish_rx_path_events: false
  diagnostic_publish_highlight_only: true
  diagnostic_publish_raw: false
```

---

## 15. Najkrótsza reguła decyzji dla `adaptive`

Jeżeli używasz **SX1276** i nie masz mocnego powodu, żeby było inaczej — **zacznij od `adaptive`**.

Na `normal` schodź dopiero wtedy, gdy:

- środowisko RF jest spokojne,
- masz mało liczników,
- `meter_window` i `summary` są stabilne,
- nie widzisz oznak busy ether.

`aggressive` traktuj jako narzędzie specjalne, a nie ustawienie „na zawsze”.
