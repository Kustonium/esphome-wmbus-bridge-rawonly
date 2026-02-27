# ESPHome wM-Bus Bridge (RAW-only)

Minimalny mostek **RF → MQTT**, który robi z ESP tylko „radio” do wM-Bus.
A minimal **RF → MQTT** bridge that turns ESP into a pure wM-Bus “radio” only.

* ESPHome odbiera telegram wM-Bus z modułu **SX1262** lub **SX1276**.
  ESPHome receives wM-Bus telegrams from **SX1262** or **SX1276**.

* Wykrywa **tryb link layer (T1/C1)**.
  It detects the **link layer mode (T1/C1)**.

* Składa ramkę i publikuje ją jako **HEX** na MQTT.
  It assembles the frame and publishes it as **HEX** over MQTT.

* Dekodowanie licznika (driver, wartości, jednostki) robisz **po stronie Home Assistant / Linux** w **wmbusmeters**.
  Meter decoding (driver, values, units) is done **on Home Assistant / Linux** using **wmbusmeters**.

To repo jest celowo „odchudzone”: **bez dekodowania na ESP**, bez dobierania sterowników, bez „kombajnu”.
This repo is intentionally “slim”: **no decoding on ESP**, no driver juggling, no “all-in-one monster”.

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

✅ publikacja telegramu jako **HEX** (payload do wmbusmeters)
✅ telegram published as **HEX** (payload for wmbusmeters)

✅ diagnostyka (opcjonalnie):
✅ diagnostics (optional):

* zliczanie zdarzeń `dropped` (np. `decode_failed`, `too_short`)
  counting `dropped` events (e.g. `decode_failed`, `too_short`)

* okresowe `summary` na MQTT
  periodic MQTT `summary`

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

  * **SX1262** (np. Heltec WiFi LoRa 32 V4.x)
    **SX1262** (e.g. Heltec WiFi LoRa 32 V4.x)

  * **SX1276** (moduły/płytki LoRa z SX1276)
    **SX1276** (LoRa modules/boards with SX1276)

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

* `examples/SX1262.yaml`
* `examples/SX1276.yaml`

Najprostszy wzór publikacji:
Minimal publish pattern:

```yaml
wmbus_radio:
  radio_type: SX1262   # albo SX1276
  # ... piny SPI/radia ...

  on_frame:
    then:
      - mqtt.publish:
          topic: "wmbus_bridge/telegram"
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

W przykładzie `examples/SX1262.yaml` jest to już uwzględnione (GPIO2/GPIO7/GPIO46).
This is already handled in `examples/SX1262.yaml` (GPIO2/GPIO7/GPIO46).

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

W `wmbus_radio` możesz włączyć publikowanie diagnostyki:
In `wmbus_radio` you can enable diagnostic publishing:

```yaml
wmbus_radio:
  diagnostic_topic: "wmbus/diag"
  diagnostic_summary_interval: 60s
  diagnostic_verbose: false
  diagnostic_publish_raw: false
```

Wtedy na `diagnostic_topic` pojawiają się JSON-y:
Then JSON messages appear on `diagnostic_topic`:

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
  `avg_ok_rssi` vs `avg_drop_rssi` – fastest way to see if it’s RF related.

- `t1.per_pct` / `c1.per_pct` (PER) – procent ramek odrzuconych w danym trybie.
  `t1.per_pct` / `c1.per_pct` (PER) – dropped packet rate per mode.

- `*_crc_pct` – ile % ramek w trybie padło na CRC DLL (bitflipy / słaby RF).
  `*_crc_pct` – how many % failed DLL CRC (bitflips / poor RF).

- `t1.sym_invalid_pct` – „quasi-BER” dla T1 (3-of-6): ile symboli 6-bit było nielegalnych.
  `t1.sym_invalid_pct` – T1 quasi-BER (3-of-6): percent of invalid 6-bit symbols.

- `hint_*` – automatyczna podpowiedź co robić (PL/EN) na podstawie bieżących statystyk.
  `hint_*` – automatic PL/EN suggestion based on the current stats.

> Uwaga: `reasons_sum_mismatch=1` oznacza błąd spójności liczenia (diagnostyka nadal działa, ale liczby mogą być niepewne).
> Note: `reasons_sum_mismatch=1` means counters mismatch (diagnostics still works but numbers may be unreliable).

#### 2) Dropped (pojedynczy drop)

#### 2) Dropped (single drop)

```json
{"event":"dropped","reason":"dll_crc_failed","mode":"C1","want":60,"got":60,"raw_got":62,"rssi":-99}
```

Opcjonalnie (gdy `diagnostic_publish_raw: true`) pojawi się też `raw(hex)` dla analizy.
Optionally (when `diagnostic_publish_raw: true`) you’ll also get `raw(hex)` for analysis.

---

---

## Dedykowany dodatek do Home Assistant (decoder)

## Dedicated Home Assistant add-on (decoder)

**PL:** Ten komponent jest zaprojektowany do pracy z dedykowanym dodatkiem HA:
**`Kustonium/homeassistant-wmbus-mqtt-bridge`**.

Dodatek subskrybuje surowe telegramy **HEX** z MQTT (np. `wmbus_bridge/telegram`), podaje je do `wmbusmeters` przez `stdin:hex`, a wynik publikuje ponownie na MQTT (JSON) + wspiera HA Discovery.

**EN:** This component is designed to work with the dedicated HA add-on:
**`Kustonium/homeassistant-wmbus-mqtt-bridge`**.

The add-on subscribes to raw **HEX** telegrams from MQTT (e.g. `wmbus_bridge/telegram`), feeds them into `wmbusmeters` via `stdin:hex`, then republishes decoded JSON to MQTT and supports HA Discovery.

Repo dodatku / Add-on repo:
`https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge`


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

W praktyce interesuje Cię tylko, żeby wmbusmeters „dostał” payload **HEX** z topicu `wmbus_bridge/telegram`.
In practice, you only need wmbusmeters to receive the **HEX** payload from `wmbus_bridge/telegram`.

---

## T1 / C1 / T2 – co z T2?

## T1 / C1 / T2 – what about T2?

Ten komponent skupia się na **T1 i C1** (najczęstsze w praktyce).
This component focuses on **T1 and C1** (most common in practice).

---

## Najczęstsze problemy

## Common issues

### 1) ESPHome nie widzi komponentu

### 1) ESPHome can’t see the component

Upewnij się, że:
Make sure that:

* repo ma katalog `components/` w root (to repo ma),
  the repo has `components/` in the root (this repo does),

* w `external_components` wskazujesz `components: [wmbus_radio]`.
  you set `components: [wmbus_radio]` in `external_components`.

### 2) Widzisz dużo „DROPPED decode_failed”

### 2) You see a lot of “DROPPED decode_failed”

To normalne w eterze, szczególnie w blokach/miastach.
That’s normal on air, especially in cities/apartment buildings.

Jeśli chcesz diagnozować:
If you want to diagnose:

* włącz `diagnostic_publish_raw: true`,
  enable `diagnostic_publish_raw: true`,

* podeślij **sam** `raw(hex)` (bez całego JSON-a) do **online analyzera** `wmbusmeters.org/analyze/…`.
  submit **only** `raw(hex)` (not the whole JSON) to the **online analyzer** at `wmbusmeters.org/analyze/…`.


### 3) wmbusmeters pokazuje “wrong key” / “payload crc failed”

### 3) wmbusmeters shows “wrong key” / “payload crc failed”

**PL:**
`wmbusmeters` potrafi wyświetlić „wrong key”, gdy telegram jest **uszkodzony radiowo** (bitflipy / ucięcie).
Dlatego ten projekt odrzuca śmieci **przed** wmbusmeters: sprawdza CRC na warstwie łącza (DLL) i nie publikuje błędnych ramek.

Co zrobić:
- sprawdź `wmbus/diag` → sekcję `c1`/`t1`:
  - jeśli `ok=0` i `crc_failed=total` przy bardzo niskim RSSI → problem RF (antena/pozycja/zakłócenia),
  - jeśli `ok>0`, a wmbusmeters nadal krzyczy → wtedy dopiero klucz/konfiguracja lub blacklist po wcześniejszych próbach.

**EN:**
`wmbusmeters` may report “wrong key” when the telegram is **RF-corrupted** (bitflips / truncated frame).
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

* ustawienia FEM (LNA/PA),
  FEM settings (LNA/PA),

* `has_tcxo` (czasem `false` działa lepiej, zależnie od płytki).
  `has_tcxo` (sometimes `false` works better depending on the board).

---

## Atrybucja

## Attribution

Projekt bazuje na doświadczeniach i fragmentach ekosystemu:
This project is based on experience and parts of the ecosystem:

* SzczepanLeon/esphome-components
* wmbusmeters/wmbusmeters

Licencja: **GPL-3.0-or-later** (patrz `LICENSE` i `NOTICE`).
License: **GPL-3.0-or-later** (see `LICENSE` and `NOTICE`).