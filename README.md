# ESPHome wM-Bus Bridge (RAW-only) – RF → HEX → MQTT → wmbusmeters (HA)

To repo zawiera **lekki komponent do ESPHome**, który robi tylko jedno zadanie:

✅ **odbiera telegramy wM-Bus z radia (SX1262 / SX1276)**  
✅ **rozpoznaje tryb (T1 / C1)**  
✅ **składa pełną ramkę i wysyła ją jako HEX na MQTT**  

❌ **nie dekoduje liczników na ESP**  
❌ **nie dobiera driverów / nie parsuje danych “do wartości”**  

Dekodowanie i dopasowanie licznika robi **wmbusmeters po stronie Home Assistant / Linux**, które czyta telegramy z MQTT.

---

## Po co to?

Jeśli masz Home Assistant i i tak używasz `wmbusmeters`, to ESP ma być tylko “radiem z MQTT”:

- mniej CPU/RAM na ESP,
- mniej problemów z kompatybilnością i aktualizacjami,
- cała logika dekodowania jest po stronie HA (łatwiej debugować i rozwijać).

---

## Wymagania

- ESPHome **2026.1.x** (zalecane)
- ESP32 / ESP32-S3 (działa stabilnie na ESP32-S3)
- Radio:
  - **SX1262** (np. Heltec WiFi LoRa 32 V4.x)
  - **SX1276** (np. moduły LoRa / płytki z SX1276)
- MQTT broker (np. Mosquitto w HA)

---

## Jak to działa (w skrócie)

1) ESP odbiera ramkę wM-Bus z radia  
2) wykrywa tryb **T1** lub **C1**  
3) składa telegram i publikuje go na MQTT jako **HEX**  
4) `wmbusmeters` na HA subskrybuje topic i **dekoduje telegramy** (już poza ESP)

Repo zawiera poprawki pod praktyczne użycie z `wmbusmeters`, m.in.:
- odfiltrowanie krótkich śmieci (np. 8 bajtów z zakłóceń),
- przygotowanie ramek w formacie przyjaznym dla dekodera po stronie HA.

---

## Instalacja (ESPHome external_components)

W konfiguracji ESPHome dodaj:

```yaml
external_components:
  - source: github://Kustonium/esphome-wmbus-bridge-rawonly@main
    components: [wmbus_radio]
    refresh: 0s
	
```	
## Atrybucja i licencja
- SzczepanLeon/esphome-components
- wmbusmeters/wmbusmeters


GPL-3.0-or-later – patrz `LICENSE` i `NOTICE`.
