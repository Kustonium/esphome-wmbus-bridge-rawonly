# Quick start: meters in Home Assistant in 15 minutes

[Wersja polska](QUICKSTART_PL.md)

Who this is for: you run Home Assistant OS (or Supervised), have a few meters at home and one radio
board. You do not need to know driver names or field names, or write YAML from scratch.

```text
meter -> ESP board (receives) -> MQTT -> HA add-on (decodes) -> Home Assistant entities
```

The board only receives. Drivers, AES keys and entities live in the add-on — adding a meter or
changing a key is a click, with no firmware reflash.

## What you need

- Home Assistant OS with the **ESPHome** add-on,
- one of the boards below,
- your meter numbers, or at least their readings from the display,
- AES keys from your utility, if your meters encrypt.

| Board | Example YAML |
|---|---|
| **XIAO ESP32-S3 + Wio-SX1262** (recommended to start) | [`examples/SX1262/XIAO ESP32 S3/xiao_esp32_s3_clean.yaml`](../examples/SX1262/XIAO%20ESP32%20S3/xiao_esp32_s3_clean.yaml) |
| Heltec WiFi LoRa 32 V4 | [`examples/SX1262/Heltec V4/`](../examples/SX1262/Heltec%20V4/) |
| Heltec WiFi LoRa 32 V4-R8 | [`examples/SX1262/Heltec V4-R8/`](../examples/SX1262/Heltec%20V4-R8/) |
| Heltec WiFi LoRa 32 V3 | [`examples/SX1262/Heltec V3/`](../examples/SX1262/Heltec%20V3/) |
| Heltec V2 / LilyGO T3 (SX1276) | [`examples/SX1276/`](../examples/SX1276/) |

Pins, RF switches and module power are already set in the examples. Do not copy them from another
board — a Heltec V4 is not a V3.

## 1. MQTT broker

Skip this if Home Assistant already has MQTT.

1. **Settings → Add-ons → Add-on store → Mosquitto broker** → Install → Start.
2. **Settings → Devices & services** → the **MQTT** integration is discovered → Configure.
3. Create a regular HA user (e.g. `mqtt`) — the board logs in with it.

## 2. wMBus MQTT Bridge add-on

[![Add repository to my Home Assistant](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FKustonium%2Fhomeassistant-wmbus-mqtt-bridge)

1. Click the button above (or add `https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge` manually).
2. Install **wMBus MQTT Bridge** → Start. Leave the meter list empty.

The add-on finds the Mosquitto broker by itself.

## 3. The board

1. In ESPHome: **+ New device** → paste the `*_clean.yaml` example for your board.
2. In **Secrets** (top right in ESPHome) fill in:

   ```yaml
   wifi_ssid: "your_network"
   wifi_password: "wifi_password"
   mqtt_broker: "192.168.1.10"   # Home Assistant IP address
   mqtt_user: "mqtt"             # the user from step 1
   mqtt_password: "mqtt_password"
   ```

3. **Install** — over USB the first time, wirelessly afterwards.
4. The board log should show `Have data ... id:XXXXXXXX` lines. Those are received meters — yours
   and your neighbours'.

## 4. Add your meters

1. In the add-on: **OPEN WEB UI** → **Received / Search** view.
2. You will see the meters in range with a suggested driver and — for unencrypted ones — the
   current value.
3. In a block of flats you will hear dozens of other meters. Type your meter's display reading into
   **Filter by value** — only matching ones stay.
4. **Add meter** → confirm the driver → enter the AES key if the meter encrypts.
5. Repeat for each meter.

Entities appear in Home Assistant by themselves (MQTT Discovery), with every field the driver
provides.

## Not working?

| Symptom | Check |
|---|---|
| No `Have data` in the board log | the right example for your board, antenna attached, board not next to the router |
| Board receives, add-on sees nothing | MQTT details in Secrets, broker running, board connected to MQTT |
| A C1 meter (e.g. some Techem) never shows up | in the board YAML change `listen_mode: t1` to `listen_mode: both` |
| Meter visible but no value | the meter encrypts — you need the AES key from your utility |
| A long telegram (three-phase electricity meter) never decodes | `long_gfsk_packets: true` on SX1262 — only then, it costs sensitivity |

More: [`START_HERE.md`](START_HERE.md) (full path and RF diagnostics),
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md), the
[add-on documentation](https://github.com/Kustonium/homeassistant-wmbus-mqtt-bridge).

Home Assistant in Docker (no add-ons): the board works the same, but you run `wmbusmeters`
yourself — see the add-on README, Docker section.
