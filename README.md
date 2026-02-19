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
  diagnostic_topic: "wmbus/diag/error"
  diagnostic_summary_interval: 60s
  diagnostic_verbose: false
  diagnostic_publish_raw: false
```

Wtedy na `diagnostic_topic` pojawiają się JSON-y:
Then JSON messages appear on `diagnostic_topic`:

* `{"event":"summary", ...}` – podsumowanie liczników (truncated/dropped itd.)
  `{"event":"summary", ...}` – counters summary (truncated/dropped etc.)

* `{"event":"dropped", "reason":"decode_failed", ...}` – pojedynczy drop (opcjonalnie z `raw`)
  `{"event":"dropped", "reason":"decode_failed", ...}` – a single drop (optionally with `raw`)

**Ważne:** `decode_failed` w dropach nie oznacza „błąd MQTT” – to zwykle:
**Important:** `decode_failed` does not mean “MQTT error” — it’s usually:

* zakłócenie,
  interference,

* ucięty telegram,
  truncated telegram,

* śmieci z eteru,
  RF garbage/noise,

* ramka nie pasująca do prostych reguł składania (np. nietypowy preamble).
  a frame that doesn’t match simple assembly rules (e.g. unusual preamble).

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

W praktyce interesuje Cię tylko, żeby wmbusmeters „dostał” payload **HEX** z topicu `wmbus_bridge/telegram`.
In practice, you only need wmbusmeters to receive the **HEX** payload from `wmbus_bridge/telegram`.

---

## T1 / C1 / T2 – co z T2?

## T1 / C1 / T2 – what about T2?

Ten komponent skupia się na **T1 i C1** (najczęstsze w praktyce).
This component focuses on **T1 and C1** (most common in practice).

Tryb „T2” bywa spotykany rzadziej i zależy od regionu/licznika.
“T2” is less common and depends on region/meter type.

Jeśli chcesz sprawdzić, czy masz T2 w eterze:
If you want to check whether you have T2 on air:

* włącz na chwilę logi `wmbus` na `debug` i obserwuj `mode: ...` w logu,
  enable `wmbus` logs to `debug` briefly and watch `mode: ...` in logs,

* albo korzystaj z diagnostyki `dropped`/`summary`.
  or use `dropped`/`summary` diagnostics.

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

* podeślij `raw(hex)` do analizy w `wmbusmeters.org/analyze/…`.
  submit `raw(hex)` for analysis at `wmbusmeters.org/analyze/…`.

### 3) Heltec V4 – słaby odbiór

### 3) Heltec V4 – poor reception

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