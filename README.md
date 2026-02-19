# ESPHome wM-Bus Bridge (RAW-only)

Minimalny mostek **RF → MQTT**, który robi z ESP tylko „radio” do wM‑Bus.

- ESPHome odbiera telegram wM‑Bus z modułu **SX1262** lub **SX1276**.
- Wykrywa **tryb link layer (T1/C1)**.
- Składa ramkę i publikuje ją jako **HEX** na MQTT.
- Dekodowanie licznika (driver, wartości, jednostki) robisz **po stronie Home Assistant / Linux** w **wmbusmeters**.

To repo jest celowo „odchudzone”: **bez dekodowania na ESP**, bez dobierania sterowników, bez „kombajnu”.

---

## Dla kogo to jest?

Dla osób, które:
- i tak używają **wmbusmeters** (np. w Home Assistant),
- chcą mieć **stabilne radio na ESP + MQTT**,
- wolą debugować/dekodować na HA (mniej bólu przy aktualizacjach, mniej RAM/CPU na ESP).

---

## Co dostajesz

✅ obsługa **SX1262** i **SX1276** (SPI)

✅ wykrywanie i obsługa ramek **T1** i **C1**

✅ publikacja telegramu jako **HEX** (payload do wmbusmeters)

✅ diagnostyka (opcjonalnie):
- zliczanie zdarzeń `dropped` (np. `decode_failed`, `too_short`)
- okresowe `summary` na MQTT
- (opcjonalnie) publikacja `raw(hex)` przy dropach

❌ brak dekodowania liczników na ESP (to robi wmbusmeters)

---

## Wymagania

- **ESPHome**: 2026.1.x+ (testowane na 2026.2.x)
- **ESP32 / ESP32‑S3** (S3 działa bardzo stabilnie)
- **MQTT broker** (np. Mosquitto w HA)
- Radio:
  - **SX1262** (np. Heltec WiFi LoRa 32 V4.x)
  - **SX1276** (moduły/płytki LoRa z SX1276)

---

## Szybki start (ESPHome)

Dodaj komponent jako `external_components`:

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s
```

Następnie skonfiguruj `wmbus_radio` i publikację telegramów na MQTT.

Repo ma gotowe przykłady:
- `examples/SX1262.yaml`
- `examples/SX1276.yaml`

Najprostszy wzór publikacji:

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

Heltec V4 ma układ FEM (tor RF) i dla dobrego RX zwykle pomaga ustawić:
- LNA ON
- PA OFF

W przykładzie `examples/SX1262.yaml` jest to już uwzględnione (GPIO2/GPIO7/GPIO46).

---

## MQTT – jakie tematy?

### Telegramy do wmbusmeters

Domyślnie w przykładach:

- `wmbus_bridge/telegram` → **HEX telegramu** (to jest to, co ma czytać wmbusmeters)

Możesz zmienić topic na własny.

### Diagnostyka (opcjonalnie)

W `wmbus_radio` możesz włączyć publikowanie diagnostyki:

```yaml
wmbus_radio:
  diagnostic_topic: "wmbus/diag/error"
  diagnostic_summary_interval: 60s
  diagnostic_verbose: false
  diagnostic_publish_raw: false
```

Wtedy na `diagnostic_topic` pojawiają się JSON-y:

- `{"event":"summary", ...}` – podsumowanie liczników (truncated/dropped itd.)
- `{"event":"dropped", "reason":"decode_failed", ...}` – pojedynczy drop (opcjonalnie z `raw`)

**Ważne:** `decode_failed` w dropach nie oznacza „błąd MQTT” – to zwykle:
- zakłócenie,
- ucięty telegram,
- śmieci z eteru,
- ramka nie pasująca do prostych reguł składania (np. nietypowy preamble).

---

## Jak podłączyć to do wmbusmeters (HA)

Idea jest prosta:
1) ESP publikuje telegramy **HEX** na MQTT.
2) `wmbusmeters` subskrybuje ten topic i dekoduje liczniki.

Jak to skonfigurować dokładnie zależy od Twojej instalacji wmbusmeters (addon/standalone) i sposobu wczytywania z MQTT.
W praktyce interesuje Cię tylko, żeby wmbusmeters „dostał” payload **HEX** z topicu `wmbus_bridge/telegram`.

---

## T1 / C1 / T2 – co z T2?

Ten komponent skupia się na **T1 i C1** (najczęstsze w praktyce).

Tryb „T2” bywa spotykany rzadziej i zależy od regionu/licznika. Jeśli chcesz sprawdzić, czy masz T2 w eterze:
- włącz na chwilę logi `wmbus` na `debug` i obserwuj `mode: ...` w logu,
- albo korzystaj z diagnostyki `dropped`/`summary`.

---

## Najczęstsze problemy

### 1) ESPHome nie widzi komponentu
Upewnij się, że:
- repo ma katalog `components/` w root (to repo ma),
- w `external_components` wskazujesz `components: [wmbus_radio]`.

### 2) Widzisz dużo „DROPPED decode_failed”
To normalne w eterze, szczególnie w blokach/miastach.
Jeśli chcesz diagnozować:
- włącz `diagnostic_publish_raw: true`,
- podeślij `raw(hex)` do analizy w `wmbusmeters.org/analyze/…`.

### 3) Heltec V4 – słaby odbiór
Sprawdź:
- piny SPI i radia (zgodne z przykładem),
- ustawienia FEM (LNA/PA),
- `has_tcxo` (czasem `false` działa lepiej, zależnie od płytki).

---

## Atrybucja

Projekt bazuje na doświadczeniach i fragmentach ekosystemu:
- SzczepanLeon/esphome-components
- wmbusmeters/wmbusmeters

Licencja: **GPL-3.0-or-later** (patrz `LICENSE` i `NOTICE`).
