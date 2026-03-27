# ESPHome wM-Bus Bridge (RAW-only)

Minimalny mostek **RF → MQTT**, który robi z ESP tylko „radio" do wM-Bus.
A minimal **RF → MQTT** bridge that turns ESP into a pure wM-Bus "radio" only.

* ESPHome odbiera telegram wM-Bus z modułu **SX1262** lub **SX1276**.
  ESPHome receives wM-Bus telegrams from **SX1262** or **SX1276**.

* Wykrywa **tryb link layer (T1/C1)**.
  It detects the **link layer mode (T1/C1)**.

* Składa ramkę i publikuje ją jako **HEX** na MQTT.
  It assembles the frame and publishes it as **HEX** over MQTT.

* (SX1262) Opcjonalny tryb `long_gfsk_packets`, który omija limit ~255B bufora RX i bywa stabilniejszy w „trudnym eterze".
  (SX1262) Optional `long_gfsk_packets` mode that bypasses the ~255B RX buffer limit and is often more stable in noisy RF environments.

* Dekodowanie licznika (driver, wartości, jednostki) robisz **po stronie Home Assistant / Linux** w **wmbusmeters**.
  Meter decoding (driver, values, units) is done **on Home Assistant / Linux** using **wmbusmeters**.

To repo jest celowo „odchudzone": **bez dekodowania na ESP**, bez dobierania sterowników, bez „kombajnu".
This repo is intentionally "slim": **no decoding on ESP**, no driver juggling, no "all-in-one monster".

---

## Dla kogo to jest? / Who is this for?

Dla osób, które:
For people who:

* i tak używają **wmbusmeters** (np. w Home Assistant),
  already use **wmbusmeters** (e.g. in Home Assistant),

* chcą mieć **stabilne radio na ESP + MQTT**,
  want a **stable ESP radio + MQTT** setup,

* wolą debugować/dekodować na HA (mniej bólu przy aktualizacjach, mniej RAM/CPU na ESP).
  prefer debugging/decoding on HA (less pain during updates, less RAM/CPU load on ESP).

---

## Co dostajesz / What you get

✅ obsługa **SX1262** i **SX1276** (SPI)
✅ **SX1262** and **SX1276** support (SPI)

✅ wykrywanie i obsługa ramek **T1** i **C1**
✅ detection and support for **T1** and **C1** frames

✅ **selektywny nasłuch** (`listen_mode: t1 / c1 / both`) — radio skupia się na wybranym trybie, diagnostyka liczy tylko pasujące ramki
✅ **selective listening** (`listen_mode: t1 / c1 / both`) — radio focuses on the chosen mode, diagnostics counts only matching frames

✅ publikacja telegramu jako **HEX** (payload do wmbusmeters)
✅ telegram published as **HEX** (payload for wmbusmeters)

✅ dostęp do **RSSI** każdej ramki w callbacku `on_frame`
✅ **RSSI** of every frame available in the `on_frame` callback

✅ **statystyki per-licznik** dla liczników z `highlight_meters` — interwał między pakietami, licznik okienkowy, RSSI, publikacja MQTT
✅ **per-meter statistics** for meters in `highlight_meters` — packet interval, windowed counter, RSSI, MQTT publish

✅ **boot event na MQTT** z informacją o typie radia i aktywnym trybie nasłuchu
✅ **MQTT boot event** with radio type and active listen mode

✅ rozbudowana diagnostyka (opcjonalnie):
✅ extended diagnostics (optional):

* `summary` dla całego eteru (tylko ramki pasujące do `listen_mode`) / global RF summary (only frames matching `listen_mode`)
* `dropped_by_reason` i `dropped_by_stage`
* `rx_path` dla problemów toru odbioru
* `highlight_meters` do wyróżniania i śledzenia wybranych liczników
* filtrowanie per-packet MQTT diag tylko do `highlight_meters`

❌ brak dekodowania liczników na ESP (to robi wmbusmeters)
❌ no meter decoding on ESP (wmbusmeters does it)

---

## Wymagania / Requirements

* **ESPHome**: 2026.1.x+ (testowane na 2026.2.x / tested on 2026.2.x)
* **ESP32 / ESP32-S3** (S3 działa bardzo stabilnie / S3 is very stable)
* **MQTT broker** (np. Mosquitto w HA / e.g. Mosquitto in HA)
* Radio: **SX1262** lub/or **SX1276**

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

Najprostszy wzór publikacji:
Minimal publish pattern:

```yaml
wmbus_radio:
  radio_type: SX1262   # albo / or SX1276
  # ... piny SPI i radia / SPI and radio pins ...

  on_frame:
    then:
      - mqtt.publish:
          topic: "wmbus_bridge/telegram"
          payload: !lambda return frame->as_hex();
```

Repo zawiera gotowe przykłady:
The repo includes ready examples:

* `examples/SX1262/HeltecV4/SX1262_full_example_LED.yaml` — Heltec V4 (SX1262), testowane / tested
* `examples/SX1276/LilygoT3S3/SX1276_T3S3_full_example.yaml` — Lilygo T3-S3 (SX1276), testowane / tested
* `examples/SX1276/HeltecV2/SX1276_Heltec_V2_full_example.yaml` — Heltec V2 (SX1276), **nie testowane sprzętowo / not hardware-tested**

---

## Heltec V4 (SX1262) – uwaga o FEM / FEM note

Heltec V4 ma układ FEM i dla dobrego RX zwykle pomaga ustawić LNA ON, PA OFF.
Heltec V4 has an RF FEM, and for good RX it usually helps to set LNA ON, PA OFF.

W przykładzie `examples/SX1262/HeltecV4/` jest to już uwzględnione (GPIO2/GPIO7/GPIO46).
This is already handled in `examples/SX1262/HeltecV4/` (GPIO2/GPIO7/GPIO46).

---

## Wszystkie opcje konfiguracji / All configuration options

### Wymagane / Required

| Klucz / Key | Opis / Description |
|---|---|
| `radio_type` | `SX1262` lub/or `SX1276` |
| `reset_pin` | GPIO pin RESET radia / radio RESET pin |
| `irq_pin` | GPIO pin IRQ/DIO1 radia / radio IRQ/DIO1 pin |
| `spi_id` / `clk_pin` / `mosi_pin` / `miso_pin` / `cs_pin` | konfiguracja SPI / SPI configuration |

### Ogólne / General

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `busy_pin` | _(brak / none)_ | GPIO pin BUSY (SX1262, zalecany / recommended) |
| `listen_mode` | `both` | Tryb nasłuchu: `t1`, `c1`, `both` — filtruje ramki i diagnostykę / Listen mode: filters frames and diagnostics |
| `on_frame` | _(brak)_ | Callback dla każdej poprawnej ramki / Callback for every valid frame |

> **`listen_mode`**: w trybie `t1` radio skupia się wyłącznie na ramkach T1 — diagnostyka (w tym `summary`) liczy tylko T1, ramki C1 są cicho odrzucane zanim trafią do liczników. Analogicznie dla `c1`. Tryb `both` zachowuje się jak dotychczas.
>
> **`listen_mode`**: in `t1` mode the radio focuses exclusively on T1 frames — diagnostics (including `summary`) counts only T1, C1 frames are silently discarded before reaching any counter. Same applies to `c1`. `both` behaves as before.

### SX1262 – opcje specyficzne / SX1262-specific options

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `dio2_rf_switch` | `true` | DIO2 jako wewnętrzny przełącznik RF / DIO2 as internal RF switch |
| `has_tcxo` | `false` | TCXO zamiast kwarcu (Heltec V4: zwykle `false`) / TCXO instead of crystal |
| `rx_gain` | `boosted` | Czułość RX: `boosted` lub `power_saving` / RX sensitivity |
| `long_gfsk_packets` | `false` | Tryb długich pakietów GFSK (AN1200.53), omija limit 255B bufora / Long GFSK packet mode, bypasses 255B buffer limit |
| `fem_en_pin` | _(brak)_ | GPIO pinu LNA enable (Heltec V4: GPIO2) |
| `fem_ctrl_pin` | _(brak)_ | GPIO pinu FEM control/RX path (Heltec V4: GPIO7) |
| `fem_pa_pin` | _(brak)_ | GPIO pinu PA enable (Heltec V4: GPIO46) |
| `clear_device_errors_on_boot` | `false` | Jednorazowe wyczyszczenie latched errors po starcie / One-shot clear of latched errors at boot |
| `publish_dev_err_after_clear` | `false` | Publikacja błędów przed/po clear do MQTT (wymaga `diagnostic_topic`) / Publish errors before/after clear to MQTT |

### Diagnostyka / Diagnostics

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `diagnostic_topic` | `"wmbus/diag"` | Topic MQTT dla diagnostyki / MQTT diagnostics topic |
| `diagnostic_verbose` | `true` | Loguj `dropped/truncated` także na serial/API / Also log to serial/API |
| `diagnostic_publish_summary` | `true` | Publikuj okresowe `summary` / Publish periodic `summary` |
| `diagnostic_publish_drop_events` | `true` | Publikuj pojedyncze eventy `dropped` / `truncated` |
| `diagnostic_publish_rx_path_events` | `true` | Publikuj eventy `rx_path` |
| `diagnostic_publish_highlight_only` | `false` | Ogranicz per-packet MQTT diag do liczników z `highlight_meters` |
| `diagnostic_publish_raw` | `true` | Dołączaj `raw(hex)` do dropów |
| `diagnostic_summary_interval` | `60s` | Interwał `summary` / Summary interval |

### Podświetlanie i statystyki per-licznik / Log highlighting and per-meter statistics

| Klucz / Key | Domyślnie / Default | Opis / Description |
|---|---|---|
| `highlight_meters` | `[]` | Lista ID liczników do wyróżnienia i śledzenia / List of meter IDs to highlight and track |
| `highlight_ansi` | `false` | Kolorowanie ANSI (zielony) w logach / ANSI color (green) in logs |
| `highlight_tag` | `"wmbus_user"` | Tag logu dla wyróżnionych liczników / Log tag for highlighted meters |
| `highlight_prefix` | `"★ "` | Prefiks przed komunikatem logu / Log message prefix |

Dla każdego licznika z `highlight_meters` komponent śledzi statystyki odbioru i publikuje je na MQTT.
For each meter in `highlight_meters` the component tracks reception statistics and publishes them to MQTT.

**Dwa niezależne triggery publikacji / Two independent publish triggers:**

| Trigger | Warunek / Condition | Pole `trigger` w payload |
|---|---|---|
| `count` | co 10 odebranych pakietów od danego licznika / every 10 received packets per meter | `"count"` |
| `time` | co 15 minut (niezależnie od liczby pakietów) / every 15 minutes (regardless of packet count) | `"time"` |

Topic: `{diagnostic_topic}/meter/{meter_id}`

```json
{
  "event": "meter_window",
  "trigger": "count",
  "id": "00089907",
  "elapsed_s": 312,
  "count_window": 10,
  "count_total": 42,
  "avg_interval_s": 30,
  "last_rssi": -59,
  "win_avg_rssi": -59
}
```

Dodatkowo przy każdym odebranym pakiecie pojawia się log i event `meter_stats` z polami lifetime:
Additionally, on every received packet a log line and `meter_stats` event with lifetime fields is emitted:

```
★ [id:00089907] count=5 interval=30.000s avg_interval=40s avg_rssi=-59dBm
```

##### Uwaga o ID liczników / Note on meter IDs

ID liczników wM-Bus to liczby dziesiętne (BCD) — wpisuj bez prefiksu `0x`.
wM-Bus meter IDs are decimal (BCD) — enter them without `0x` prefix.

```yaml
wmbus_radio:
  radio_type: SX1262
  highlight_meters:
    - "00089907"
    - "12345678"
  highlight_ansi: true
```

---

## Metody dostępne w `on_frame` / Methods available in `on_frame`

| Metoda / Method | Zwraca / Returns | Opis / Description |
|---|---|---|
| `frame->as_hex()` | `std::string` | Telegram HEX po usunięciu CRC DLL / HEX telegram after DLL CRC removal |
| `frame->as_raw()` | `std::vector<uint8_t>` | Surowe bajty ramki / Raw frame bytes |
| `frame->as_rtlwmbus()` | `std::string` | Format rtl-wmbus (tryb/czas/rssi/hex) |
| `frame->rssi()` | `int8_t` | RSSI w dBm |
| `frame->link_mode()` | `LinkMode` | Tryb: `T1` / `C1` |
| `frame->format()` | `std::string` | Format bloku, np. `"A"`, `"B"` |

```yaml
on_frame:
  mark_as_handled: true
  then:
    - mqtt.publish:
        topic: "wmbus_bridge/telegram"
        payload: !lambda return frame->as_hex();
```

---

## MQTT – jakie tematy? / MQTT – which topics?

### Telegramy / Telegrams

* `wmbus_bridge/telegram` → **HEX telegramu** (do wmbusmeters / for wmbusmeters)

### Diagnostyka / Diagnostics

Topic bazowy: `diagnostic_topic` (domyślnie `wmbus/diag` / default `wmbus/diag`)

#### 1) `boot` — jednorazowo po starcie / once after boot

Publikowany zaraz po nawiązaniu połączenia MQTT. Zawiera typ radia i aktywny tryb nasłuchu.
Published as soon as MQTT connection is established. Contains radio type and active listen mode.

Topic: `{diagnostic_topic}/boot` (retained), dodatkowo na `{diagnostic_topic}` (not retained)

```json
{"event":"boot","radio":"SX1276","listen_mode":"T1+C1 (both, 3:1 bias)","uptime_ms":8120}
```

#### 2) `summary` — co interval / every interval

Liczy tylko ramki pasujące do aktywnego `listen_mode`.
Counts only frames matching the active `listen_mode`.

```json
{
  "event": "summary",
  "total": 25,
  "ok": 22,
  "truncated": 0,
  "dropped": 3,
  "crc_failed": 0,
  "listen_mode": "both",
  "hint_code": "OK",
  "hint_pl": "wygląda dobrze"
}
```

**Jak czytać `summary` / How to read `summary`:**

* `total` — ramki które przeszły filtr `listen_mode` i dotarły do parsera / frames that passed `listen_mode` filter and reached the parser
* `ok` — ramki poprawnie dostarczone do końca / frames successfully delivered end-to-end
* `avg_ok_rssi` vs `avg_drop_rssi` — szybki sygnał czy problem wygląda na RF / quick signal whether the issue looks like RF
* `t1.per_pct` / `c1.per_pct` — procent ramek odrzuconych w danym trybie / dropped packet rate per mode
* `hint_*` — automatyczna podpowiedź PL/EN na podstawie bieżących statystyk / automatic PL/EN hint based on current stats

#### 3) `dropped` / `truncated` — per pakiet / per packet

```json
{"event":"dropped","reason":"decode_failed","stage":"t1_decode3of6","mode":"T1","rssi":-86}
```

#### 4) `meter_window` — statystyki per-licznik / per-meter statistics

Topic: `{diagnostic_topic}/meter/{meter_id}`

Dla każdego licznika z `highlight_meters`, dwa triggery: co 10 pakietów (`"count"`) i co 15 minut (`"time"`).
For each meter in `highlight_meters`, two triggers: every 10 packets (`"count"`) and every 15 minutes (`"time"`).

```json
{"event":"meter_window","trigger":"time","id":"00089907","elapsed_s":900,"count_window":3,"count_total":42,"avg_interval_s":127,"last_rssi":-59,"win_avg_rssi":-60}
```

#### 5) `dev_err_cleared` — SX1262, jednorazowo po starcie / once after boot

```json
{"event":"dev_err_cleared","before":4,"before_hex":"0004","after":0,"after_hex":"0000"}
```

---

## Praktyczny profil diagnostyki / Practical diagnostics profile

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
    - "00089908"
    - "12345678"
```

---

## Dedykowany dodatek do Home Assistant / Dedicated Home Assistant add-on

**`Kustonium/homeassistant-wmbus-mqtt-bridge`**

Subskrybuje surowe telegramy HEX z MQTT, podaje je do `wmbusmeters` przez `stdin:hex`, publikuje wynik jako JSON + wspiera HA Discovery.
Subscribes to raw HEX telegrams from MQTT, feeds them into `wmbusmeters` via `stdin:hex`, republishes decoded JSON and supports HA Discovery.

`https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge`

---

## Najczęstsze problemy / Common issues

### 1) ESPHome nie widzi komponentu / ESPHome can't see the component

Sprawdź czy `external_components` ma `components: [wmbus_radio]` i czy repo ma katalog `components/` w root.
Check that `external_components` has `components: [wmbus_radio]` and the repo has a `components/` directory in root.

### 2) Dużo `DROPPED decode_failed` / Many `DROPPED decode_failed`

Normalne w gęstym eterze (bloki, miasto). Włącz `diagnostic_publish_raw: true` i podeślij `raw(hex)` do `wmbusmeters.org/analyze/`.
Normal in dense RF environments (apartments, cities). Enable `diagnostic_publish_raw: true` and submit `raw(hex)` to `wmbusmeters.org/analyze/`.

### 3) wmbusmeters pokazuje `wrong key` / `payload crc failed`

Sprawdź `wmbus/diag` → sekcję `c1`/`t1`: jeśli `ok=0` i `crc_failed=total` przy niskim RSSI → problem RF (antena/pozycja). Jeśli `ok>0` — klucz lub blacklist.
Check `wmbus/diag` → `c1`/`t1`: if `ok=0` and `crc_failed=total` at low RSSI → RF issue (antenna/placement). If `ok>0` — key/config or blacklist.

### 4) W trybie `listen_mode: c1` `summary` pokazuje `total=0` / In `listen_mode: c1` summary shows `total=0`

Poprawne zachowanie — diagnostyka liczy tylko ramki C1. Jeśli w pobliżu nie ma urządzeń C1, `total` będzie 0 lub bardzo niskie.
Expected behavior — diagnostics counts only C1 frames. If there are no C1 devices nearby, `total` will be 0 or very low.

### 5) Heltec V4 – słaby odbiór / poor reception

Sprawdź piny SPI i radia, ustawienia FEM (`fem_en_pin`, `fem_ctrl_pin`, `fem_pa_pin`), `has_tcxo`.
Check SPI and radio pins, FEM settings (`fem_en_pin`, `fem_ctrl_pin`, `fem_pa_pin`), `has_tcxo`.

### 6) Losowe restarty (SX1262) / Random resets (SX1262)

Zasilacz 5V 2A+ i porządny kabel. Na czas testów `reboot_timeout: 0s`.
5V 2A+ adapter and a decent cable. For testing use `reboot_timeout: 0s`.

---

## Atrybucja / Attribution

Projekt bazuje na doświadczeniach i fragmentach ekosystemu:
This project is based on experience and parts of the ecosystem:

* SzczepanLeon/esphome-components
* wmbusmeters/wmbusmeters

Licencja: **GPL-3.0-or-later** (patrz `LICENSE` i `NOTICE` / see `LICENSE` and `NOTICE`).
