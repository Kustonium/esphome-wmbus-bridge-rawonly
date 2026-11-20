# ESP-NOW repeater: a receiver without the home network

[Polska wersja](README_PL.md)

> **Status: in progress.** Tested on one bench pair (Heltec V4 receiver + ESP32-C3 bridge). The other node files are build-checked in CI only; range tests are still to come.

The receiver sits next to the meters (basement, garage, stairwell), out of reach of
your Wi-Fi. A second ESP32 inside the network takes its frames over ESP-NOW and
publishes them to MQTT on its behalf. The add-on sees the receiver as an ordinary
board. Nothing to type in on either side: no MAC addresses, no channel.

```
meters --868 MHz--> NODE (wM-Bus radio, no network) --ESP-NOW 2.4 GHz--> BRIDGE (home Wi-Fi) --MQTT--> add-on
```

Tested on a bench with ~70 real meters (~1800 frames/h in the daytime): 7.5 h and
13 344 frames with no loss; the bridge unplugged for 100 s without a frame lost.
The buffer was then raised from 100 to 1500 frames after a 15-minute bridge
outage cost 320 frames with the small one.

## Files

| File | What it is |
|---|---|
| `node_*.yaml` | the receiver (NODE) for each example board, generated from its `examples/.../*_clean.yaml` |
| `repeater_node_common.yaml` | the part shared by every node: hotspot, status page, firmware upload, buffer sensors |
| `repeater_node.h` | node logic: pairing, buffer, delivery |
| `bridge_esp32c3.yaml` | the BRIDGE, any ESP32 / S3 / C3 on the home network, no radio needed |
| `repeater_bridge.h` | bridge logic: accepting nodes, publishing, confirmations |

Copy the node YAML, `repeater_node_common.yaml` and `repeater_node.h` into one
folder in ESPHome; the bridge YAML with `repeater_bridge.h` likewise.

`secrets.yaml` needs, on top of the usual entries:

```yaml
repeater_ap_password: "12+ characters"    # the node's hotspot (WPA2)
repeater_web_username: "admin"            # login of the node's page
repeater_web_password: "12+ characters"   # also guards the firmware upload
```

There is no node file for the LilyGO T3 V3 TCXO: its board example does not
validate (`cs_pin: GPIO7` is a flash pin on the ESP32), so a node generated from
it would not either.

## Setting it up

1. **Flash the bridge** and check its log: Wi-Fi and MQTT connected.
2. **Flash the node.** After this flash the node is **no longer on your network**:
   ESPHome / Home Assistant cannot update it any more. Updates go through its own
   hotspot (below) or USB. Flash it while it is still within reach of the PC or
   over USB.
3. Power the node. Within seconds the bridge log shows `new node …` and
   `node ACCEPTED …`, then `<name> seq=1 …` for every frame. In the add-on the
   node appears under its own name (`wmbus/<node name>/…`).

The node's own `esphome.name` is the board name the add-on shows.

## On site, with a phone

The node always runs a hotspot named after the board (`esphome-wmbus-heltec-v4`,
…). Connect a phone or laptop and open `http://192.168.4.1` (it asks for the page login):

| Entity | Meaning |
|---|---|
| **Bridge** | `CONNECTED - channel 6, link -72 dBm`, `LOST for N s - X frames waiting`, `bridge WITHOUT MQTT`, `looking for the bridge…`, `NOT FOUND…` |
| **Last meters** | the last 5 meters received, with RSSI and age |
| **Link to bridge** | ESP-NOW signal from the bridge |
| **Frames delivered** | published to MQTT by the bridge |
| **Buffered / Buffer peak / Buffer capacity** | frames waiting now / the highest so far / the size |
| **Frames lost (buffer full / too long)** | the only real loss counter |
| **Send failures (retried)** | traffic on the link, not losses |
| **OTA Update** | upload a firmware file (ESPHome: *Install → Manual download → OTA format*). After the upload the page stays on "Update Successful!"; go back to `http://192.168.4.1/` |

Walk the cellar with the phone to find a spot where both the meters and the link
to the bridge work.

Rough link thresholds: above −80 dBm comfortable, −80…−88 dBm fine, below
−90 dBm send failures rise; as long as "Frames lost" stays at 0 nothing is lost.

## How it works

**Pairing.** The node hops channels 1–13 and broadcasts `HELLO`. The bridge sits on
the router's channel and answers with that channel, whether the node is accepted,
the current time and whether it has MQTT. The node keeps the channel (also in
flash, so the next boot starts there) and sends `HELLO` every 15 s as a keepalive.
If the router changes channel, the node finds it again within 2 minutes. While a
phone is on the hotspot the node does not change channel.

**Accepting nodes.** The first node that says `HELLO` is accepted and kept in
flash. Others are listed as "Other nodes heard". Buttons: *Accept strongest node*,
*Forget node*. A neighbour's node is not forwarded.

**Delivery.** Every frame goes into the node's buffer first. It leaves the buffer
only when the bridge confirms that it **published the frame to MQTT** - the ESP-NOW
radio ACK alone is not trusted, because a bridge that has just rebooted receives
frames before it has MQTT. Without MQTT the bridge confirms nothing and the node
keeps buffering (`bridge WITHOUT MQTT`). Three failures in a row mark the link lost
at once; a probe every 2 s brings it back. A confirmation lost on the way makes the
node send the frame again; the bridge recognises it by number and confirms without
publishing twice. The bridge publishes frames with QoS 1, so a Wi-Fi/MQTT link
that drops for a moment does not swallow them: the MQTT client keeps each frame
until the broker acknowledges it and sends it again after reconnecting.

**Buffer.** 1500 frames in PSRAM (~370 kB): about 50 minutes of daytime traffic
from ~70 meters, hours at night. Boards without PSRAM get 100 frames in RAM. A
full buffer drops its oldest frame.

**Numbering.** Frames are numbered from 1. A lost frame shows as a gap in `seq` in
the add-on; a node restart starts a new `boot_id`. The bridge also sets
`received_at` to the time the node received the frame (the node gets its clock
from the bridge), so a frame that waited in the buffer keeps its real time.

**Statistics.** The bridge publishes `wmbus/repeater/link` every 60 s:
`rx`, `lost` (seq gaps), `dup` (retries recognised), `foreign`, link RSSI, and the
node's own counters `a_dropped`, `a_send_fail`, `a_queue` (they survive a bridge
restart).

**In the add-on.** The node adds its uptime, frame count, average RSSI, radio chip
and listen mode to its `HELLO`; the bridge publishes them every 60 s as
`wmbus/<node>/health` (the same pulse an ordinary board sends) with
`"via":"espnow"`, its own name and the link RSSI. The add-on then shows the node
as alive and its tile reads *behind ESP-NOW bridge*, e.g. `SX1262 · t1 · bridge:
esphome-wmbus-repeater-bridge · link -70 dBm`. Chip and listen mode come from
`radio_chip` / `radio_listen_mode` in the node's `substitutions`, which its
`wmbus_radio` uses as well.

The node also sends the component's configuration report and settings (the JSON
a board with MQTT publishes on `<diag>/config` and `<diag>/settings`) in parts
when its buffer is empty: after start, after the link comes back and every
30 min. The bridge publishes them retained, so the add-on shows the node's
configuration and its settings panel. A setting changed in the add-on
(`wmbus/<node>/set/<key>`) goes through the bridge to the node, which stores it
and restarts, exactly like a board with MQTT; the bridge repeats the command with
the next `HELLO`s (up to 3 times) until the node reports its settings again.

**Screenshots.** Both boards have an *Anonymize* switch: meter IDs in the log and
on the page are masked (same method as `set_log_anonymize`, see
[DIAGNOSTIC](../../docs/DIAGNOSTIC.md)). MQTT gets the real IDs.

## Range

ESP-NOW runs at 1 Mbit/s here, the most sensitive plain Wi-Fi rate. From a
measured link you can estimate: every doubling of distance costs ~6 dB in the open
and ~9 dB indoors; a partition wall 3–5 dB, a brick wall 6–10 dB, a reinforced
concrete floor 15–25 dB. In practice: within a house through a few walls, one
floor through a stairwell; an external 2.4 GHz antenna on either board (XIAO
ESP32-S3, some T-Beam versions) adds 3–6 dB.

## Security

- **Hotspot:** WPA2 with `repeater_ap_password`. It encrypts the radio link, so
  the page's plain HTTP is not readable without that password. Use 12+ characters.
- **Page login:** `repeater_web_username` / `repeater_web_password`, digest
  authentication: the password is not sent in clear, and it guards the
  **firmware upload** too, so knowing the hotspot password alone is not enough to
  flash the node. Without the page password the node can only be updated over USB.
- **No HTTPS:** ESPHome's web server does not offer TLS (and a self-signed
  certificate would only trade this for a browser warning). The two passwords
  above are the protection.
- **The node is not on your network:** its only way out is ESP-NOW to the bridge,
  which forwards frames and nothing else, so a compromised node is no path into
  your LAN.
- **ESP-NOW is not encrypted:** it carries the frames the meters already transmit
  in the clear; AES-encrypted meters stay encrypted end to end.

## Limits

- **Board lights in the add-on:** only *Alive* works; *Radio*, *Keeps up* and
  *Reception* stay grey, because the node sends no diagnostic summaries.
- **One node per bridge.** The bridge forwards one accepted node.
- **Long frames need ESPHome 2026.11 on the bridge.** A wM-Bus frame is up to
  256 B; with the 11 B header that is more than the 250 B of classic ESP-NOW, so
  the bridge uses ESP-NOW v2 (`espnow: max_payload_size: 300`, ESPHome 2026.11+;
  the node's ESP-IDF 5.4+ already speaks v2). With an older bridge or the default
  250 the node counts frames longer than 239 B as lost ("Frames lost") and says so
  in its log, instead of blocking its buffer.
- Needs the component with `repeater:` (`wmbus_radio` with `repeater: true`
  publishes nothing over MQTT; see [CONFIG_REFERENCE_MINIMAL](../../docs/CONFIG_REFERENCE_MINIMAL.md)).
