# ESPHome wM-Bus Bridge (RAW-only)

Minimalny mostek **RF → MQTT**, który robi z ESP tylko „radio" do wM-Bus.
A minimal **RF → MQTT** bridge that turns ESP into a pure wM-Bus radio.

## Co ten projekt robi / What this project does

* ESPHome odbiera telegramy wM-Bus z radia **SX1262** albo **SX1276**.
  ESPHome receives wM-Bus telegrams from **SX1262** or **SX1276**.

* Rozpoznaje i składa ramki **T1** oraz **C1** — dual-path parser z fallbackiem i łagodnym precheck'iem.
  It recognizes and assembles **T1** and **C1** frames — dual-path parser with fallback and soft precheck.

* Publikuje gotowy telegram jako **HEX** na MQTT.
  It publishes the final telegram as **HEX** over MQTT.

* Opcjonalnie publikuje diagnostykę RF/parsowania na MQTT.
  Optionally, it publishes RF/parser diagnostics to MQTT.

* Dekodowanie liczników (driver, pola, jednostki) zostaje po stronie **Home Assistant / Linux** w **wmbusmeters**.
  Meter decoding (driver, fields, units) stays on **Home Assistant / Linux** using **wmbusmeters**.

To repo jest celowo odchudzone: **bez dekodowania na ESP**, bez dobierania driverów i bez kombajnu „all-in-one".
This repo is intentionally slim: **no decoding on ESP**, no driver juggling, and no all-in-one monster.

---

## Dla kogo to jest? / Who is this for?

Dla osób, które:
For people who:

* już używają **wmbusmeters** (np. w Home Assistant),
  already use **wmbusmeters** (e.g. in Home Assistant),

* chcą mieć **stabilny odbiór na ESP + MQTT**,
  want **stable ESP reception + MQTT**,

* wolą debugować i dekodować po stronie HA/Linux zamiast upychać wszystko w ESP.
  prefer debugging and decoding on HA/Linux instead of pushing everything into ESP.

---

## Co dostajesz / What you get

✅ obsługa **SX1262** i **SX1276** (SPI)
✅ **SX1262** and **SX1276** support (SPI)

✅ obsługa ramek **T1** i **C1**
✅ **T1** and **C1** frame handling

✅ **dual-path parser z fallbackiem** — jeśli preferowana ścieżka (T1/C1) zawiedzie, automatycznie próbuje drugiej; `pick_better_failure_()` wybiera diagnostykę z najdalej zaawansowanego etapu
✅ **dual-path parser with fallback** — if the preferred path (T1/C1) fails, automatically tries the other; `pick_better_failure_()` selects diagnostics from the furthest parsing stage

✅ **łagodny precheck T1** — minimum 12 bajtów raw (wcześniej 60B); krótsze pakiety trafiają do parsera zamiast być odrzucane z miejsca
✅ **soft T1 precheck** — minimum 12 raw bytes (previously 60B); shorter packets reach the parser instead of being dropped immediately

✅ publikacja telegramów jako **HEX** do MQTT
✅ **HEX** telegram publishing to MQTT

✅ `on_frame` z dostępem do **RSSI**, trybu link layer i gotowej ramki
✅ `on_frame` with **RSSI**, link mode and final frame access

✅ rozbudowana diagnostyka (opcjonalna):
✅ extended diagnostics (optional):

* `summary` dla całego eteru / global RF summary
* `dropped_by_reason` i `dropped_by_stage`
* `rx_path` dla problemów toru odbioru
* `highlight_meters` do wyróżniania wybranych liczników
* filtrowanie per-packet MQTT diag tylko do `highlight_meters`

❌ brak dekodowania liczników na ESP
❌ no meter decoding on ESP

---

## Wymagania / Requirements

* **ESPHome**: 2026.1.x+ (testowane na 2026.2.x)
  **ESPHome**: 2026.1.x+ (tested on 2026.2.x)

* **ESP32 / ESP32-S3** (S3 działa bardzo stabilnie / S3 is very stable)

* **MQTT broker** (np. Mosquitto / e.g. Mosquitto)

* Radio / Radio: **SX1262** lub/or **SX1276**

---

## Płytki i status testów / Boards and test status

### Przetestowane przeze mnie / Tested by me

* **Heltec WiFi LoRa 32 V4.x — SX1262**
* **LilyGO T3-S3 — SX1276**

### Przykład pinów, bez testów sprzętowych / Pin-based example, not hardware-tested by me

* **Heltec WiFi LoRa 32 V2 — SX1276**

To ważne: przykład dla **Heltec V2** jest oparty na pinach i logice komponentu, ale sama płytka **nie była przeze mnie testowana sprzętowo**.
Important: the **Heltec V2** example is based on pin mapping and component logic, but the board itself **was not hardware-tested by me**.

---

## Szybki start / Quick start

Dodaj komponent jako `external_components`:
Add the component via `external_components`:

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s
```

> Do testów możesz używać `@main`. Do bardziej stabilnego wdrożenia lepiej przypiąć konkretny tag/release.
> For testing you can use `@main`. For a more stable deployment, pin a specific tag/release.

Zalecany wzorzec topicu dla telegramów:
Recommended topic pattern for telegrams:

```
wmbus_bridge/<node>/telegram
```

Na przykład / For example:

* `wmbus_bridge/heltec/telegram`
* `wmbus_bridge/lilygo/telegram`

Dzięki temu decoder po stronie HA może słuchać wildcardem:
This lets the HA-side decoder subscribe using a wildcard:

```
wmbus_bridge/+/telegram
```

Najprostszy wzór publikacji:
Minimal publish pattern:

```yaml
wmbus_radio:
  radio_type: SX1262   # albo / or SX1276
  # ... piny SPI i radia / SPI and radio pins ...

  on_frame:
    then:
      - mqtt.publish:
          topic: "wmbus_bridge/heltec/telegram"
          payload: !lambda |-
            return frame->as_hex();
```

Repo zawiera gotowe przykłady:
The repo includes ready examples:

* `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` — Heltec V4 (SX1262), testowane / tested
* `examples/SX1276/LilygoT3S3/SX1276_T3S3_full_example.yaml` — Lilygo T3-S3 (SX1276), testowane / tested
* `examples/SX1276/HeltecV2/SX1276_Heltec_V2_full_example.yaml` — Heltec V2 (SX1276), **nie testowane sprzętowo / not hardware-tested**

---

## Heltec V4 (SX1262) – uwaga o FEM / FEM note

Heltec V4 ma tor RF z FEM i dla dobrego RX zwykle pomaga ustawienie:
Heltec V4 has an RF front-end with FEM and for good RX it usually helps to set:

* **LNA ON**
* **PA OFF**

W przykładzie `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` jest to już uwzględnione (GPIO2/GPIO7/GPIO46).
This is already handled in `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` (GPIO2/GPIO7/GPIO46).

---

## Opcje konfiguracji / Configuration options

### Wymagane / Required

| Klucz / Key | Opis / Description |
|---|---|
| `radio_type` | `SX1262` albo/or `SX1276` |
| `reset_pin` | pin RESET radia / radio RESET pin |
| `irq_pin` | pin IRQ/DIO1 radia / radio IRQ/DIO1 pin |
| `spi_id` / `clk_pin` / `mosi_pin` / `miso_pin` / `cs_pin` | konfiguracja SPI / SPI configuration |

### Ogólne / General

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `busy_pin` | _(brak / none)_ | pin BUSY (SX1262, zalecany / recommended) |
| `on_frame` | _(brak / none)_ | callback dla każdej poprawnej ramki / callback for every valid frame |

### SX1262 – opcje specyficzne / SX1262-specific options

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `dio2_rf_switch` | `true` | DIO2 jako wewnętrzny przełącznik RF / DIO2 as internal RF switch |
| `has_tcxo` | `false` | TCXO zamiast kwarcu (Heltec V4: zwykle `false`) / TCXO instead of crystal |
| `rx_gain` | `boosted` | czułość RX: `boosted` albo/or `power_saving` |
| `long_gfsk_packets` | `false` | tryb długich pakietów GFSK (AN1200.53), omija limit 255B bufora / long GFSK packet mode, bypasses 255B buffer limit |
| `fem_en_pin` | _(brak)_ | pin LNA enable (Heltec V4: GPIO2) |
| `fem_ctrl_pin` | _(brak)_ | pin FEM control / RX path (Heltec V4: GPIO7) |
| `fem_pa_pin` | _(brak)_ | pin PA enable (Heltec V4: GPIO46) |
| `clear_device_errors_on_boot` | `false` | jednokrotne wyczyszczenie latched errors po starcie / one-shot clear of latched errors at boot |
| `publish_dev_err_after_clear` | `false` | publikacja błędów przed/po clear do MQTT (wymaga `diagnostic_topic`) / publish errors before/after clear to MQTT |

### Diagnostyka / Diagnostics

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `diagnostic_topic` | `"wmbus/diag"` | topic MQTT dla diagnostyki / MQTT diagnostics topic |
| `diagnostic_verbose` | `true` | logowanie `dropped/truncated` także na serial/API / also log `dropped/truncated` to serial/API |
| `diagnostic_publish_summary` | `true` | publikuj okresowe `summary` / publish periodic `summary` |
| `diagnostic_publish_drop_events` | `true` | publikuj pojedyncze eventy `dropped` / `truncated` |
| `diagnostic_publish_rx_path_events` | `true` | publikuj eventy `rx_path` (IRQ timeout, read_failed, queue_send_failed) |
| `diagnostic_publish_highlight_only` | `false` | ogranicz per-packet MQTT diag do liczników z `highlight_meters` |
| `diagnostic_publish_raw` | `true` | dołącz `raw(hex)` do dropów / include `raw(hex)` in drop events |
| `diagnostic_summary_interval` | `60s` | interwał `summary` / summary interval |

### Podświetlanie logów / Log highlighting

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `highlight_meters` | `[]` | lista ID liczników do wyróżnienia w logach ESP / list of meter IDs to highlight in ESP logs |
| `highlight_ansi` | `false` | kolorowanie ANSI (zielony) w logach / ANSI color (green) in logs |
| `highlight_tag` | `"wmbus_user"` | tag logu dla wyróżnionych liczników / log tag for highlighted meters |
| `highlight_prefix` | `"★ "` | prefiks logu / log prefix |

Przykład / Example:

```yaml
wmbus_radio:
  radio_type: SX1262
  highlight_meters:
    - "00089907"
    - "12345678"
  highlight_ansi: true
```

Wyróżnianie działa w logach ESP (serial/API) i może też filtrować per-packet MQTT diag, jeśli ustawisz `diagnostic_publish_highlight_only: true`. Nie filtruje `on_frame` ani głównej publikacji telegramów.
Highlighting works in ESP logs (serial/API) and can also filter per-packet MQTT diagnostics when `diagnostic_publish_highlight_only: true`. It does not filter `on_frame` or your main telegram publish.

---

## Metody dostępne w `on_frame` / Methods available in `on_frame`

W callbacku `on_frame` dostępny jest wskaźnik `frame` typu `Frame *`:
In the `on_frame` callback, a `frame` pointer of type `Frame *` is available:

| Metoda / Method | Typ / Type | Opis / Description |
|---|---|---|
| `frame->as_hex()` | `std::string` | telegram HEX po usunięciu CRC DLL / HEX telegram after DLL CRC removal |
| `frame->as_raw()` | `std::vector<uint8_t>` | surowe bajty ramki (bez CRC DLL) / frame bytes (DLL CRC stripped) |
| `frame->as_rtlwmbus()` | `std::string` | format rtl-wmbus (tryb/czas/rssi/hex) / rtl-wmbus format |
| `frame->rssi()` | `int8_t` | RSSI w dBm |
| `frame->link_mode()` | `LinkMode` | tryb / mode (`T1` / `C1`) |
| `frame->format()` | `std::string` | format bloku, np. `"A"` / `"B"` |

Przykład / Example:

```yaml
on_frame:
  then:
    - mqtt.publish:
        topic: "wmbus_bridge/heltec/telegram"
        payload: !lambda return frame->as_hex();

    - mqtt.publish:
        topic: "wmbus_bridge/heltec/rssi"
        payload: !lambda return to_string((int) frame->rssi());

    # Format rtl-wmbus (jeśli coś go konsumuje / if anything consumes it)
    # - mqtt.publish:
    #     topic: "wmbus_bridge/heltec/rtlwmbus"
    #     payload: !lambda return frame->as_rtlwmbus();
```

### `mark_as_handled`

`mark_as_handled: true` sygnalizuje komponentowi, że ramka została obsłużona (wpływa na logi diagnostyczne).
`mark_as_handled: true` signals to the component that the frame was handled (affects diagnostic logs).

```yaml
on_frame:
  mark_as_handled: true
  then:
    - mqtt.publish:
        topic: "wmbus_bridge/heltec/telegram"
        payload: !lambda return frame->as_hex();
```

---

## MQTT – jakie tematy? / MQTT – which topics?

### Telegramy do wmbusmeters / Telegrams for wmbusmeters

Zalecany wzorzec topicu:
Recommended topic pattern:

* `wmbus_bridge/<node>/telegram` → **HEX telegramu** / **HEX telegram**

Decoder po stronie HA może słuchać na:
The HA-side decoder can subscribe to:

* `wmbus_bridge/+/telegram`

Możesz zmienić wzorzec na własny — ważne tylko, żeby wmbusmeters dostał payload **HEX**.
You can use your own topic pattern — the only requirement is that wmbusmeters receives the **HEX** payload.

### Diagnostyka / Diagnostics

Praktyczny, cichy profil do codziennego użycia:
A practical, quiet profile for daily use:

```yaml
wmbus_radio:
  diagnostic_topic: "wmbus/diag"
  diagnostic_summary_interval: 60s
  diagnostic_verbose: false
  diagnostic_publish_summary: true
  diagnostic_publish_drop_events: true
  diagnostic_publish_rx_path_events: false
  diagnostic_publish_highlight_only: true
  diagnostic_publish_raw: false

  highlight_meters:
    - "00089907"
    - "12345678"
```

Co to daje / What this gives you:

* `summary` dalej liczy cały eter / `summary` still counts all RF traffic
* per-packet eventy `dropped` / `truncated` lecą tylko dla liczników z `highlight_meters`
* `rx_path` nie spamuje MQTT
* payloady są mniejsze, bo bez `raw(hex)`

Dodatkowo (SX1262), żeby opublikować latched błędy radia po starcie:
Additionally (SX1262), to publish latched radio errors after boot:

```yaml
wmbus_radio:
  clear_device_errors_on_boot: true
  publish_dev_err_after_clear: true
  diagnostic_topic: "wmbus/diag"
```

#### 1) `summary` (co interval / every interval)

```json
{
  "event": "summary",
  "total": 32,
  "ok": 25,
  "truncated": 0,
  "dropped": 7,
  "crc_failed": 2,
  "crc_fail_pct": 6,
  "drop_pct": 21,
  "trunc_pct": 0,
  "avg_ok_rssi": -80,
  "avg_drop_rssi": -97,
  "t1": {
    "total": 31,
    "ok": 24,
    "dropped": 7,
    "per_pct": 22,
    "crc_failed": 0,
    "crc_pct": 0,
    "avg_ok_rssi": -80,
    "avg_drop_rssi": -96,
    "sym_total": 5780,
    "sym_invalid": 13,
    "sym_invalid_pct": 0
  },
  "c1": {
    "total": 1,
    "ok": 1,
    "dropped": 0,
    "per_pct": 0,
    "crc_failed": 0,
    "crc_pct": 0
  },
  "dropped_by_reason": {
    "too_short": 0,
    "decode_failed": 7,
    "dll_crc_failed": 0,
    "unknown_preamble": 0,
    "l_field_invalid": 0,
    "unknown_link_mode": 0,
    "other": 0
  },
  "dropped_by_stage": {
    "precheck": 0,
    "t1_decode3of6": 7,
    "t1_l_field": 0,
    "t1_length_check": 0,
    "c1_precheck": 0,
    "c1_preamble": 0,
    "c1_suffix": 0,
    "c1_l_field": 0,
    "c1_length_check": 0,
    "dll_crc_first": 0,
    "dll_crc_mid": 0,
    "dll_crc_final": 0,
    "dll_crc_b1": 0,
    "dll_crc_b2": 0,
    "link_mode": 0,
    "other": 0
  },
  "rx_path": {
    "irq_timeout": 0,
    "preamble_read_failed": 0,
    "t1_header_read_failed": 0,
    "payload_size_unknown": 0,
    "payload_read_failed": 0,
    "queue_send_failed": 0
  },
  "reasons_sum": 7,
  "reasons_sum_mismatch": 0,
  "hint_code": "OK",
  "hint_en": "Looks good.",
  "hint_pl": "Wygląda dobrze."
}
```

Jak czytać `summary` praktycznie / How to read `summary` in practice:

* `total` – wszystko, co radio zobaczyło / everything the radio saw
* `ok` – ramki poprawnie dostarczone do końca / frames successfully delivered end-to-end
* `avg_ok_rssi` vs `avg_drop_rssi` – szybki sygnał czy problem wygląda na RF / quick signal whether the problem looks like RF
* `t1.per_pct` / `c1.per_pct` – procent ramek odrzuconych w danym trybie / dropped packet rate per mode
* `*_crc_pct` – ile % ramek w trybie padło na CRC DLL (bitflipy / słaby RF) / how many % failed DLL CRC
* `t1.sym_invalid_pct` – quasi-BER dla T1 (3-of-6) / T1 quasi-BER
* `dropped_by_stage` – pokazuje, na którym etapie najczęściej padają ramki / shows at which stage frames most often fail
* `rx_path` – pokazuje problemy jeszcze przed parserem / shows problems before the parser

> Większe `total` nie zawsze znaczy lepiej — liczy się relacja `ok` do `dropped`. Różnice między urządzeniami mogą wynikać z całego toru odbiorczego (RF, antena, chip), nie tylko z parsera.
> Higher `total` does not always mean better — what matters is the `ok` to `dropped` ratio. Differences between devices can come from the full receive chain (RF, antenna, chip), not only the parser.

> `reasons_sum_mismatch=1` oznacza błąd spójności liczników (diagnostyka nadal działa, ale liczby mogą być niepewne).
> `reasons_sum_mismatch=1` means a counter consistency error (diagnostics still works but numbers may be unreliable).

#### 2) `dropped` (pojedynczy drop / single drop)

```json
{"event":"dropped","reason":"decode_failed","stage":"t1_decode3of6","mode":"T1","want":0,"got":0,"raw_got":134,"decoded_len":0,"final_len":0,"dll_crc_removed":0,"suffix_ignored":0,"rssi":-86,"detail":"symbols_total=178 symbols_invalid=2 raw_len=134"}
```

Pole `detail` zawiera dodatkowy kontekst etapu który zawiódł. Jeśli parser użył ścieżki fallback, pojawi się `"fallback_used=1"`.
The `detail` field contains extra context from the failing stage. If the parser used the fallback path, `"fallback_used=1"` will appear.

Opcjonalnie (gdy `diagnostic_publish_raw: true`, domyślnie) pojawi się też `raw(hex)` dla analizy.
Optionally (when `diagnostic_publish_raw: true`, which is the default) you'll also get `raw(hex)` for analysis.

#### 3) `dev_err_cleared` (SX1262, jednorazowo po starcie / once after boot)

```json
{"event":"dev_err_cleared","before":4,"before_hex":"0004","after":0,"after_hex":"0000"}
```

---

## Dodatek Home Assistant / Home Assistant add-on

Ten komponent jest projektowany do współpracy z dodatkiem:
This component is designed to work with the add-on:

**`Kustonium/homeassistant-wmbus-mqtt-bridge`**

Dodatek subskrybuje surowe telegramy **HEX** z MQTT (`wmbus_bridge/+/telegram`), podaje je do `wmbusmeters` przez `stdin:hex`, a wynik publikuje ponownie na MQTT jako JSON i wspiera HA Discovery.
The add-on subscribes to raw **HEX** telegrams from MQTT (`wmbus_bridge/+/telegram`), feeds them into `wmbusmeters` via `stdin:hex`, republishes decoded JSON to MQTT, and supports HA Discovery.

Repo dodatku / Add-on repo:
`https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge`

---

## Jak to spiąć z wmbusmeters? / How to connect it to wmbusmeters?

1. ESP publikuje telegramy **HEX** na MQTT.
   ESP publishes **HEX** telegrams to MQTT.

2. `wmbusmeters` subskrybuje topic z telegramami i dekoduje liczniki.
   `wmbusmeters` subscribes to the telegram topic and decodes meters.

W praktyce interesuje Cię tylko to, żeby `wmbusmeters` dostał payload **HEX** z topicu:
In practice, you only need `wmbusmeters` to receive the **HEX** payload from a topic like:

```
wmbus_bridge/+/telegram
```

---

## T1 / C1 / T2 – co z T2? / What about T2?

Ten komponent skupia się na **T1** i **C1** — najczęstszych i najbardziej praktycznych przypadkach.
This component focuses on **T1** and **C1** — the most common and practical cases.

---

## Najczęstsze problemy / Common issues

### 1) ESPHome nie widzi komponentu / ESPHome can't see the component

Upewnij się, że:
Make sure that:

* repo ma katalog `components/` w root (to repo ma),
  the repo has `components/` in root (this repo does),

* w `external_components` używasz `components: [wmbus_radio]`.
  you set `components: [wmbus_radio]` in `external_components`.

### 2) Widzisz dużo `DROPPED decode_failed` / You see many `DROPPED decode_failed`

To bywa normalne w gęstym eterze, szczególnie w blokach i miastach.
That can be normal in dense RF environments, especially in cities/apartment buildings.

Jeśli chcesz diagnozować głębiej / If you want deeper diagnostics:

* sprawdź `dropped_by_stage` w `summary` / check `dropped_by_stage` in `summary`
* sprawdź `rx_path` / check `rx_path`
* ewentualnie włącz `diagnostic_publish_raw: true` i podeślij `raw(hex)` do online analyzera `wmbusmeters.org/analyze/…`

### 3) wmbusmeters pokazuje `wrong key` / `payload crc failed`

`wmbusmeters` potrafi wyświetlić „wrong key", gdy telegram jest **uszkodzony radiowo** (bitflipy / ucięcie). Ten projekt odrzuca śmieci **przed** wmbusmeters: sprawdza CRC na warstwie łącza (DLL) i nie publikuje błędnych ramek.
`wmbusmeters` may report "wrong key" when the telegram is **RF-corrupted** (bitflips / truncated). This project drops garbage **before** wmbusmeters by validating DLL CRC and rejecting bad frames.

Co zrobić / What to do:

* sprawdź `wmbus/diag` → sekcję `c1`/`t1` / check `wmbus/diag` → `c1`/`t1` section:
  * jeśli `ok=0` i `crc_failed=total` przy bardzo niskim RSSI → problem RF (antena/pozycja)
    if `ok=0` and `crc_failed=total` at very low RSSI → RF issue (antenna/placement)
  * jeśli `ok>0`, a wmbusmeters nadal krzyczy → klucz/konfiguracja lub blacklist po wcześniejszych próbach
    if `ok>0` but wmbusmeters still complains → key/config or a previous blacklist

### 4) Heltec V4 – słaby odbiór / poor reception

Sprawdź / Check:

* piny SPI i radia (zgodne z przykładem) / SPI and radio pins (match the example)
* ustawienia FEM (LNA/PA) — `fem_en_pin`, `fem_ctrl_pin`, `fem_pa_pin`
* `has_tcxo` (czasem `false` działa lepiej, zależnie od płytki / sometimes `false` works better depending on the board)

### 5) Losowe restarty / rozłączenia API (SX1262) / Random resets / API disconnects

Najczęstsza przyczyna: zasilanie z portu USB komputera, słaby kabel lub zbyt „miękki" zasilacz.
Most common cause: powering from a PC USB port, a poor cable, or a weak power supply.

Co zwykle pomaga / What usually helps:

* zasilacz 5V 2A+ i krótki, porządny kabel / 5V 2A+ adapter and a short, decent cable
* na czas testów wyłącz automatyczne restarty Wi-Fi/API (`reboot_timeout: 0s`) / for testing, disable Wi-Fi/API auto-reboots

---

## Atrybucja / Attribution

Projekt bazuje na doświadczeniach i fragmentach ekosystemu:
This project is based on experience and parts of the ecosystem:

* SzczepanLeon/esphome-components
* wmbusmeters/wmbusmeters

Licencja: **GPL-3.0-or-later** (patrz `LICENSE` i `NOTICE` / see `LICENSE` and `NOTICE`).
License: **GPL-3.0-or-later** (see `LICENSE` and `NOTICE`).
