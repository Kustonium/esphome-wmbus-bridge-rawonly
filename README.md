# ESPHome wM-Bus Bridge (RAW-only)

Minimalny mostek **RF → MQTT**, który robi z ESP tylko „radio" do wM-Bus.
A minimal **RF → MQTT** bridge that turns ESP into a pure wM-Bus "radio" only.

* ESPHome odbiera telegram wM-Bus z modułu **SX1262** lub **SX1276**.
  ESPHome receives wM-Bus telegrams from **SX1262** or **SX1276**.

* Wykrywa **tryb link layer (T1/C1)** z dual-path parserem i fallbackiem.
  It detects the **link layer mode (T1/C1)** using a dual-path parser with fallback.

* Składa ramkę i publikuje ją jako **HEX** na MQTT.
  It assembles the frame and publishes it as **HEX** over MQTT.

* (SX1262) Opcjonalny tryb `long_gfsk_packets`, który omija limit ~255B bufora RX i bywa stabilniejszy w „trudnym eterze".
  (SX1262) Optional `long_gfsk_packets` mode that bypasses the ~255B RX buffer limit and is often more stable in noisy RF environments.

* Dekodowanie licznika (driver, wartości, jednostki) robisz **po stronie Home Assistant / Linux** w **wmbusmeters**.
  Meter decoding (driver, values, units) is done **on Home Assistant / Linux** using **wmbusmeters**.

To repo jest celowo „odchudzone": **bez dekodowania na ESP**, bez dobierania sterowników, bez „kombajnu".
This repo is intentionally "slim": **no decoding on ESP**, no driver juggling, no "all-in-one monster".

---

## Dla kogo to jest?

## Who is this for?

Dla osób, które:
For people who:

* i tak używają **wmbusmeters** (np. w Home Assistant),
  already use **wmbusmeters** (e.g. in Home Assistant),

* chcą mieć **stabilne radio na ESP + MQTT**,
  want a **stable ESP radio + MQTT** setup,

* wolą debugować/dekodować na HA (mniej bólu przy aktualizacjach, mniej RAM/CPU na ESP).
  prefer debugging/decoding on HA (less pain during updates, less RAM/CPU load on ESP).

---

## Co dostajesz

## What you get

✅ obsługa **SX1262** i **SX1276** (SPI)
✅ **SX1262** and **SX1276** support (SPI)

✅ wykrywanie i obsługa ramek **T1** i **C1**
✅ detection and support for **T1** and **C1** frames

✅ **dual-path parser z fallbackiem** — jeśli preferowana ścieżka (T1/C1) zawiedzie, automatycznie próbuje drugiej; `pick_better_failure_()` wybiera diagnostykę z najdalej zaawansowanego etapu
✅ **dual-path parser with fallback** — if the preferred path (T1/C1) fails, automatically tries the other; `pick_better_failure_()` selects diagnostics from the furthest parsing stage

✅ **łagodny precheck T1** — minimum 12 bajtów raw (wcześniej 60B); krótsze pakiety trafiają do parsera zamiast być odrzucane z miejsca
✅ **soft T1 precheck** — minimum 12 raw bytes (previously 60B); shorter packets reach the parser instead of being dropped immediately

✅ publikacja telegramu jako **HEX** (payload do wmbusmeters)
✅ telegram published as **HEX** (payload for wmbusmeters)

✅ dostęp do **RSSI** każdej ramki w callbacku `on_frame`
✅ **RSSI** of every frame available in the `on_frame` callback

✅ diagnostyka (opcjonalnie):
✅ diagnostics (optional):

* zliczanie zdarzeń `dropped` z rozbiciem po `reason` i `stage`
  counting `dropped` events broken down by `reason` and `stage`

* okresowe `summary` na MQTT (globalne dla całego eteru)
  periodic MQTT `summary` (global for all RF traffic)

* osobne eventy `rx_path` (opcjonalnie) dla problemów toru odbioru
  separate `rx_path` events (optional) for receive-path problems

* (opcjonalnie) publikacja `raw(hex)` przy dropach
  (optional) publish `raw(hex)` on drops

❌ brak dekodowania liczników na ESP (to robi wmbusmeters)
❌ no meter decoding on ESP (wmbusmeters does it)

---

## Wymagania

## Requirements

* **ESPHome**: 2026.1.x+ (testowane na 2026.2.x)
  **ESPHome**: 2026.1.x+ (tested on 2026.2.x)

* **ESP32 / ESP32-S3** (S3 działa bardzo stabilnie)
  **ESP32 / ESP32-S3** (S3 is very stable)

* **MQTT broker** (np. Mosquitto w HA)
  **MQTT broker** (e.g. Mosquitto in HA)

* Radio:
  Radio:

  * **SX1262** (np. Heltec WiFi LoRa 32 V4.x) — testowane
    **SX1262** (e.g. Heltec WiFi LoRa 32 V4.x) — tested

  * **SX1276** (np. Lilygo T3-S3, moduły LoRa z SX1276) — testowane
    **SX1276** (e.g. Lilygo T3-S3, LoRa modules with SX1276) — tested

  * **SX1276** (Heltec WiFi LoRa 32 V2) — tylko przykład YAML, **nie testowane przez autora**
    **SX1276** (Heltec WiFi LoRa 32 V2) — example YAML only, **not tested by the author**

---

## Szybki start (ESPHome)

## Quick start (ESPHome)

Dodaj komponent jako `external_components`:
Add the component via `external_components`:

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s
```

Następnie skonfiguruj `wmbus_radio` i publikację telegramów na MQTT.
Then configure `wmbus_radio` and publish telegrams to MQTT.

Repo ma gotowe przykłady:
The repo includes ready examples:

* `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` — Heltec V4 (SX1262), testowane
  `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` — Heltec V4 (SX1262), tested

* `examples/SX1276/LilygoT3S3/SX1276_T3S3_full_example.yaml` — Lilygo T3-S3 (SX1276), testowane
  `examples/SX1276/LilygoT3S3/SX1276_T3S3_full_example.yaml` — Lilygo T3-S3 (SX1276), tested

* `examples/SX1276/HeltecV2/SX1276_Heltec_V2_full_example.yaml` — Heltec V2 (SX1276), **nie testowane przez autora**
  `examples/SX1276/HeltecV2/SX1276_Heltec_V2_full_example.yaml` — Heltec V2 (SX1276), **not tested by the author**

Najprostszy wzór publikacji:
Minimal publish pattern:

```yaml
wmbus_radio:
  radio_type: SX1262   # albo SX1276
  # SX1262: zalecane w trudnym eterze / dłuższych ramkach:
  # long_gfsk_packets: true
  # ... piny SPI/radia ...

  on_frame:
    then:
      - mqtt.publish:
          topic: "wmbus_bridge/lilygo/telegram"
          payload: !lambda |-
            return frame->as_hex();
```

### Heltec V4 (SX1262) – ważna uwaga o FEM

### Heltec V4 (SX1262) – important FEM note

Heltec V4 ma układ FEM (tor RF) i dla dobrego RX zwykle pomaga ustawić:
Heltec V4 has an RF FEM, and for good RX it usually helps to set:

* LNA ON
  LNA ON

* PA OFF
  PA OFF

W przykładzie `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` jest to już uwzględnione (GPIO2/GPIO7/GPIO46).
This is already handled in `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` (GPIO2/GPIO7/GPIO46).

---

## Wszystkie opcje konfiguracji

## All configuration options

### Wymagane / Required

| Klucz / Key | Opis / Description |
|---|---|
| `radio_type` | `SX1262` lub `SX1276` |
| `reset_pin` | GPIO pin RESET radia / radio RESET pin |
| `irq_pin` | GPIO pin IRQ/DIO1 radia / radio IRQ/DIO1 pin |
| `spi_id` / `clk_pin` / `mosi_pin` / `miso_pin` / `cs_pin` | SPI bus configuration |

### Ogólne / General

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `busy_pin` | _(brak / none)_ | GPIO pin BUSY (SX1262, zalecany / recommended) |
| `on_frame` | _(brak)_ | Callback wywoływany dla każdej poprawnej ramki / Callback for every valid frame |

### SX1262 – opcje specyficzne / SX1262-specific options

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `dio2_rf_switch` | `true` | DIO2 jako wewnętrzny przełącznik RF / DIO2 as internal RF switch |
| `has_tcxo` | `false` | TCXO zamiast kwarcu (Heltec V4: zwykle `false`) / TCXO instead of crystal |
| `rx_gain` | `boosted` | Czułość RX: `boosted` lub `power_saving` / RX sensitivity: `boosted` or `power_saving` |
| `long_gfsk_packets` | `false` | Tryb długich pakietów GFSK (AN1200.53), omija limit 255B bufora / Long GFSK packet mode, bypasses 255B buffer limit |
| `fem_en_pin` | _(brak)_ | GPIO pinu LNA enable (Heltec V4: GPIO2) |
| `fem_ctrl_pin` | _(brak)_ | GPIO pinu FEM control/RX path (Heltec V4: GPIO7) |
| `fem_pa_pin` | _(brak)_ | GPIO pinu PA enable (Heltec V4: GPIO46) |
| `clear_device_errors_on_boot` | `false` | Jednorazowe wyczyszczenie rejestrów błędów SX1262 po starcie / One-shot clear of SX1262 latched error registers at boot |
| `publish_dev_err_after_clear` | `false` | Opublikuj wartości błędów przed/po wyczyszczeniu na MQTT (wymaga `diagnostic_topic`) / Publish error values before/after clear to MQTT (requires `diagnostic_topic`) |

### Diagnostyka / Diagnostics

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `diagnostic_topic` | `"wmbus/name/diag"` | Topic MQTT dla diagnostyki / MQTT topic for diagnostics |
| `diagnostic_verbose` | `true` | Loguj `dropped/truncated` także na serial/API / Also log `dropped/truncated` to serial/API |
| `diagnostic_publish_summary` | `true` | Publikuj okresowe `summary` / Publish periodic `summary` |
| `diagnostic_publish_drop_events` | `true` | Publikuj pojedyncze eventy `dropped` / `truncated` / Publish per-packet `dropped` / `truncated` events |
| `diagnostic_publish_rx_path_events` | `true` | Publikuj eventy `rx_path` (IRQ timeout, read_failed, queue_send_failed) / Publish `rx_path` events |
| `diagnostic_publish_highlight_only` | `false` | Ogranicz per-packet MQTT diag do liczników z `highlight_meters` / Limit per-packet MQTT diagnostics to `highlight_meters` |
| `diagnostic_publish_raw` | `true` | Dołączaj `raw(hex)` do dropów / Include `raw(hex)` in drop events |
| `diagnostic_summary_interval` | `60s` | Jak często wysyłać podsumowanie / How often to send summary |

### Podświetlanie logów (opcjonalne) / Log highlighting (optional)

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `highlight_meters` | `[]` | Lista ID liczników do wyróżnienia w logach ESP / List of meter IDs to highlight in ESP logs |
| `highlight_ansi` | `false` | Kolorowanie ANSI (zielony) w logach / ANSI color (green) in logs |
| `highlight_tag` | `"wmbus_user"` | Tag logu dla wyróżnionych liczników / Log tag for highlighted meters |
| `highlight_prefix` | `"★ "` | Prefiks przed komunikatem logu / Prefix before the log message |

Przykład / Example:

```yaml
wmbus_radio:
  radio_type: SX1262
  highlight_meters:
    - "00089907"
    - "12345678"
  highlight_ansi: true
```

Wyróżnienie działa w logach ESP (serial/API) i może też zostać użyte jako filtr dla per-packet MQTT diag, jeśli ustawisz `diagnostic_publish_highlight_only: true`. Nie filtruje `on_frame` ani głównej publikacji telegramów.
Highlighting works in ESP logs (serial/API) and can also be reused as a filter for per-packet MQTT diagnostics when `diagnostic_publish_highlight_only: true`. It does not filter `on_frame` or your main telegram publish.

---

## Metody dostępne w `on_frame`

## Methods available in `on_frame`

W callbacku `on_frame` dostępny jest wskaźnik `frame` typu `Frame *`:
In the `on_frame` callback, a `frame` pointer of type `Frame *` is available:

| Metoda / Method | Zwraca / Returns | Opis / Description |
|---|---|---|
| `frame->as_hex()` | `std::string` | Telegram jako HEX po usunięciu CRC DLL (do wmbusmeters) / Telegram as HEX after DLL CRC removal (for wmbusmeters) |
| `frame->as_raw()` | `std::vector<uint8_t>` | Surowe bajty ramki (bez CRC DLL) / Raw frame bytes (DLL CRC stripped) |
| `frame->as_rtlwmbus()` | `std::string` | Format rtl-wmbus (tryb/czas/rssi/hex) / rtl-wmbus format (mode/time/rssi/hex) |
| `frame->rssi()` | `int8_t` | RSSI w dBm / RSSI in dBm |
| `frame->link_mode()` | `LinkMode` | Tryb (T1/C1) / Mode (T1/C1) |
| `frame->format()` | `std::string` | Format bloku (np. `"A"`, `"B"`) / Block format (e.g. `"A"`, `"B"`) |

Przykłady lambda:
Lambda examples:

```yaml
on_frame:
  then:
    # Telegram HEX dla wmbusmeters
    - mqtt.publish:
        topic: "wmbus_bridge/name/telegram"
        payload: !lambda return frame->as_hex();

    # RSSI na osobnym topicu
    - mqtt.publish:
        topic: "wmbus_bridge/rssi"
        payload: !lambda return to_string((int) frame->rssi());

    # Format rtl-wmbus (jeśli coś go konsumuje)
    # - mqtt.publish:
    #     topic: "wmbus_bridge/rtlwmbus"
    #     payload: !lambda return frame->as_rtlwmbus();
```

### `mark_as_handled`

Opcja `mark_as_handled: true` w `on_frame` sygnalizuje komponentowi, że ramka została obsłużona (logi diagnostyczne).
The `mark_as_handled: true` option in `on_frame` signals to the component that the frame was handled (affects diagnostic logs).

```yaml
on_frame:
  mark_as_handled: true
  then:
    - mqtt.publish:
        topic: "wmbus_bridge/name/telegram"
        payload: !lambda return frame->as_hex();
```

---

## MQTT – jakie tematy?

## MQTT – which topics?

### Telegramy do wmbusmeters

### Telegrams for wmbusmeters

Domyślnie w przykładach:
Default in examples:

* `wmbus_bridge/telegram` → **HEX telegramu** (to jest to, co ma czytać wmbusmeters)
  `wmbus_bridge/telegram` → **telegram HEX** (this is what wmbusmeters should read)

Możesz zmienić topic na własny.
You can change the topic to your own.

### Diagnostyka (opcjonalnie)

### Diagnostics (optional)

W `wmbus_radio` możesz sterować nie tylko samą diagnostyką, ale też tym, **ile** z niej ma lecieć na MQTT.
In `wmbus_radio` you can control not only diagnostics itself, but also **how much** of it should be published to MQTT.

Praktyczny, "cichy" profil do codziennego użycia:
A practical, "quiet" profile for daily use:

```yaml
wmbus_radio:
  diagnostic_topic: "wmbus/name/diag"
  diagnostic_summary_interval: 120s
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

Co to daje:
What this gives you:

* `summary` dalej liczy cały eter,
  `summary` still counts all RF traffic,

* pojedyncze eventy `dropped` / `truncated` lecą tylko dla liczników z `highlight_meters`,
  per-packet `dropped` / `truncated` events are published only for meters from `highlight_meters`,

* `rx_path` nie spamuje MQTT,
  `rx_path` does not spam MQTT,

* payloady są mniejsze, bo bez `raw(hex)`.
  payloads are smaller because they do not include `raw(hex)`.

Dodatkowo (SX1262), żeby opublikować latched błędy radia po starcie:
Additionally (SX1262), to publish latched radio errors after boot:

```yaml
wmbus_radio:
  clear_device_errors_on_boot: true
  publish_dev_err_after_clear: true
  diagnostic_topic: "wmbus/name/diag"
```

#### 1) Summary (co interval)

#### 1) Summary (every interval)

```json
{
  "event": "summary",
  "total": 30,
  "ok": 23,
  "truncated": 0,
  "dropped": 7,
  "crc_failed": 2,
  "crc_fail_pct": 6,
  "drop_pct": 23,
  "trunc_pct": 0,

  "avg_ok_rssi": -74,
  "avg_drop_rssi": -97,

  "t1": {
    "total": 28,
    "ok": 23,
    "dropped": 5,
    "per_pct": 17,
    "crc_failed": 0,
    "crc_pct": 0,
    "avg_ok_rssi": -74,
    "avg_drop_rssi": -96,

    "sym_total": 1234,
    "sym_invalid": 12,
    "sym_invalid_pct": 1
  },
  "c1": {
    "total": 2,
    "ok": 0,
    "dropped": 2,
    "per_pct": 100,
    "crc_failed": 2,
    "crc_pct": 100,
    "avg_ok_rssi": 0,
    "avg_drop_rssi": -99
  },

  "dropped_by_reason": {
    "too_short": 0,
    "decode_failed": 5,
    "dll_crc_failed": 2,
    "unknown_preamble": 0,
    "l_field_invalid": 0,
    "unknown_link_mode": 0,
    "other": 0
  },

  "dropped_by_stage": {
    "precheck": 0,
    "t1_decode3of6": 5,
    "t1_l_field": 0,
    "t1_length_check": 0,
    "c1_precheck": 0,
    "c1_preamble": 0,
    "c1_suffix": 0,
    "c1_l_field": 0,
    "c1_length_check": 0,
    "dll_crc_first": 2,
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

  "hint_code": "C1_WEAK_SIGNAL",
  "hint_en": "C1 frames fail DLL CRC at very low RSSI; improve antenna/placement.",
  "hint_pl": "Ramy C1 padają na CRC DLL przy bardzo niskim RSSI; popraw antenę/pozycję."
}
```

**Jak czytać summary (praktycznie):**
**How to read the summary (practical):**

- `avg_ok_rssi` vs `avg_drop_rssi` – najszybsza odpowiedź czy problem jest radiowy.
  `avg_ok_rssi` vs `avg_drop_rssi` – fastest way to see if it's RF related.

- `t1.per_pct` / `c1.per_pct` (PER) – procent ramek odrzuconych w danym trybie.
  `t1.per_pct` / `c1.per_pct` (PER) – dropped packet rate per mode.

- `*_crc_pct` – ile % ramek w trybie padło na CRC DLL (bitflipy / słaby RF).
  `*_crc_pct` – how many % failed DLL CRC (bitflips / poor RF).

- `t1.sym_invalid_pct` – „quasi-BER" dla T1 (3-of-6): ile symboli 6-bit było nielegalnych.
  `t1.sym_invalid_pct` – T1 quasi-BER (3-of-6): percent of invalid 6-bit symbols.

- `hint_*` – automatyczna podpowiedź co robić (PL/EN) na podstawie bieżących statystyk.
  `hint_*` – automatic PL/EN suggestion based on the current stats.

> Uwaga: `reasons_sum_mismatch=1` oznacza błąd spójności liczenia (diagnostyka nadal działa, ale liczby mogą być niepewne).
> Note: `reasons_sum_mismatch=1` means counters mismatch (diagnostics still works but numbers may be unreliable).

#### 2) Dropped (pojedynczy drop)

#### 2) Dropped (single drop)

```json
{"event":"dropped","reason":"decode_failed","stage":"t1_decode3of6","mode":"T1","want":0,"got":0,"raw_got":134,"decoded_len":0,"final_len":0,"dll_crc_removed":0,"suffix_ignored":0,"rssi":-86,"detail":"symbols_total=178 symbols_invalid=2 raw_len=134"}
```

Pole `detail` zawiera dodatkowy kontekst etapu który zawiódł. Jeśli parser użył ścieżki fallback (np. pakiet wyglądał jak C1, ale zdekodował się jako T1), pojawi się `"fallback_used=1"`.
The `detail` field contains extra context from the failing stage. If the parser used the fallback path (e.g. packet looked like C1 but decoded as T1), `"fallback_used=1"` will appear.

Opcjonalnie (gdy `diagnostic_publish_raw: true`, domyślnie) pojawi się też `raw(hex)` dla analizy.
Optionally (when `diagnostic_publish_raw: true`, which is the default) you'll also get `raw(hex)` for analysis.

#### 3) dev_err_cleared (SX1262, jednorazowo po starcie)

#### 3) dev_err_cleared (SX1262, once after boot)

Gdy `publish_dev_err_after_clear: true`:
When `publish_dev_err_after_clear: true`:

```json
{"event":"dev_err_cleared","before":4,"before_hex":"0004","after":0,"after_hex":"0000"}
```

---

## Dedykowany dodatek do Home Assistant (decoder)

## Dedicated Home Assistant add-on (decoder)

**PL:** Ten komponent jest zaprojektowany do pracy z dedykowanym dodatkiem HA:
**`Kustonium/homeassistant-wmbus-mqtt-bridge`**.

Dodatek subskrybuje surowe telegramy **HEX** z MQTT (np. `wmbus_bridge/name/telegram`), podaje je do `wmbusmeters` przez `stdin:hex`, a wynik publikuje ponownie na MQTT (JSON) + wspiera HA Discovery.

**EN:** This component is designed to work with the dedicated HA add-on:
**`Kustonium/homeassistant-wmbus-mqtt-bridge`**.

The add-on subscribes to raw **HEX** telegrams from MQTT (e.g. `wmbus_bridge/name/telegram`), feeds them into `wmbusmeters` via `stdin:hex`, then republishes decoded JSON to MQTT and supports HA Discovery.

Repo dodatku / Add-on repo:
`https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge`

---

## Jak podłączyć to do wmbusmeters (HA)

## How to connect this to wmbusmeters (HA)

Idea jest prosta:
The idea is simple:

1. ESP publikuje telegramy **HEX** na MQTT.
   ESP publishes **HEX** telegrams to MQTT.

2. `wmbusmeters` subskrybuje ten topic i dekoduje liczniki.
   `wmbusmeters` subscribes to that topic and decodes meters.

Jak to skonfigurować dokładnie zależy od Twojej instalacji wmbusmeters (addon/standalone) i sposobu wczytywania z MQTT.
Exact configuration depends on your wmbusmeters setup (addon/standalone) and how you feed data from MQTT.

W praktyce interesuje Cię tylko, żeby wmbusmeters „dostał" payload **HEX** z topicu `wmbus_bridge/+/telegram`.
In practice, you only need wmbusmeters to receive the **HEX** payload from `wmbus_bridge/+/telegram`.

---

## T1 / C1 / T2 – co z T2?

## T1 / C1 / T2 – what about T2?

Ten komponent skupia się na **T1 i C1** (najczęstsze w praktyce).
This component focuses on **T1 and C1** (most common in practice).

---

## Najczęstsze problemy

## Common issues

### 1) ESPHome nie widzi komponentu

### 1) ESPHome can't see the component

Upewnij się, że:
Make sure that:

* repo ma katalog `components/` w root (to repo ma),
  the repo has `components/` in the root (this repo does),

* w `external_components` wskazujesz `components: [wmbus_radio]`.
  you set `components: [wmbus_radio]` in `external_components`.

### 2) Widzisz dużo „DROPPED decode_failed"

### 2) You see a lot of "DROPPED decode_failed"

To normalne w eterze, szczególnie w blokach/miastach.
That's normal on air, especially in cities/apartment buildings.

Jeśli chcesz diagnozować:
If you want to diagnose:

* włącz `diagnostic_publish_raw: true` (domyślnie jest włączone),
  enable `diagnostic_publish_raw: true` (it's on by default),

* podeślij **sam** `raw(hex)` (bez całego JSON-a) do **online analyzera** `wmbusmeters.org/analyze/…`.
  submit **only** `raw(hex)` (not the whole JSON) to the **online analyzer** at `wmbusmeters.org/analyze/…`.


### 3) wmbusmeters pokazuje "wrong key" / "payload crc failed"

### 3) wmbusmeters shows "wrong key" / "payload crc failed"

**PL:**
`wmbusmeters` potrafi wyświetlić „wrong key", gdy telegram jest **uszkodzony radiowo** (bitflipy / ucięcie).
Dlatego ten projekt odrzuca śmieci **przed** wmbusmeters: sprawdza CRC na warstwie łącza (DLL) i nie publikuje błędnych ramek.

Co zrobić:
- sprawdź `wmbus/diag` → sekcję `c1`/`t1`:
  - jeśli `ok=0` i `crc_failed=total` przy bardzo niskim RSSI → problem RF (antena/pozycja/zakłócenia),
  - jeśli `ok>0`, a wmbusmeters nadal krzyczy → wtedy dopiero klucz/konfiguracja lub blacklist po wcześniejszych próbach.

**EN:**
`wmbusmeters` may report "wrong key" when the telegram is **RF-corrupted** (bitflips / truncated frame).
This project drops garbage **before** wmbusmeters by validating DLL CRC and rejecting bad frames.

What to do:
- check `wmbus/diag` → `c1`/`t1`:
  - if `ok=0` and `crc_failed=total` at very low RSSI → RF issue (antenna/placement/interference),
  - if `ok>0` but wmbusmeters still complains → key/config or a previous blacklist.

### 4) Heltec V4 – słaby odbiór

### 4) Heltec V4 – poor reception

Sprawdź:
Check:

* piny SPI i radia (zgodne z przykładem),
  SPI and radio pins (match the example),

* ustawienia FEM (LNA/PA) — `fem_en_pin`, `fem_ctrl_pin`, `fem_pa_pin`,
  FEM settings (LNA/PA) — `fem_en_pin`, `fem_ctrl_pin`, `fem_pa_pin`,

* `has_tcxo` (czasem `false` działa lepiej, zależnie od płytki).
  `has_tcxo` (sometimes `false` works better depending on the board).


### 5) Losowe restarty / rozłączenia API (SX1262)

### 5) Random resets / API disconnects (SX1262)

Najczęstsza przyczyna to zasilanie z portu USB komputera, słaby kabel albo zbyt "miękki" zasilacz.
The most common cause is powering from a PC USB port, a poor cable, or a weak power supply.

Co zwykle pomaga:
What usually helps:

* zasilacz 5V 2A+ i krótki, porządny kabel,
  a 5V 2A+ adapter and a short, decent cable,

* na czas testów wyłącz automatyczne restarty Wi-Fi/API (`reboot_timeout: 0s`) i dodaj `uptime`, żeby odróżnić reboot od chwilowej utraty połączenia,
  for testing, disable Wi‑Fi/API auto-reboots (`reboot_timeout: 0s`) and add `uptime` to tell reboots from temporary link drops.

---

## Atrybucja

## Attribution

Projekt bazuje na doświadczeniach i fragmentach ekosystemu:
This project is based on experience and parts of the ecosystem:

* SzczepanLeon/esphome-components
* wmbusmeters/wmbusmeters

Licencja: **GPL-3.0-or-later** (patrz `LICENSE` i `NOTICE`).
License: **GPL-3.0-or-later** (see `LICENSE` and `NOTICE`).
