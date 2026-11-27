# Security

[Polska wersja](SECURITY_PL.md)

## Reporting a vulnerability

Please report security problems privately through GitHub: **Security → Report a vulnerability** on this repository. Do not put details of an unfixed vulnerability in a public issue. This is a spare-time project (see [SUPPORT.md](SUPPORT.md)): I answer as soon as I can, without a guaranteed response time.

## What the board sends, and in what form

`wmbus_radio` is RAW-only: the board receives wM-Bus frames and publishes them to MQTT; it does not decode meters and **holds no meter AES keys**. Decoding, and the keys, stay with `wmbusmeters` outside the board.

What goes over MQTT:

| Topic | Content |
|---|---|
| `wmbus/<board>/telegram` | the frame as received (hex): meter ID, manufacturer and type always readable; the readings themselves readable for unencrypted meters, encrypted for meters that use AES |
| `wmbus/<board>/rx` | per-frame metadata: meter ID, RSSI, reception time, frame checksum |
| `wmbus/<board>/diag/...` | board health and radio statistics (only what `diagnostic_mode` enables) |
| `wmbus/<board>/rssi/<id>` | latest RSSI per meter (only with `publish_rssi: true`) |

Frames from unencrypted meters show consumption over time, which can reveal when someone is at home. That data is as sensitive as the meters it comes from.

## Recommended setup

- **Keep the MQTT broker on the local network.** Do not expose port 1883 to the internet.
- **Use broker authentication**: `username` / `password` from `secrets.yaml`, as all examples do.
- **TLS** to the broker is ESPHome's standard `mqtt:` component, not part of `wmbus_radio`; see the ESPHome MQTT documentation (CA certificate, port 8883). It costs RAM (a TLS record buffer is on the order of 16 KB, see [BUFFER_QOS_TIMESTAMP](docs/BUFFER_QOS_TIMESTAMP.md)), so keep `mqtt_buffer_size` moderate on boards without PSRAM.
- **Since ESPHome 2026.11, `skip_cert_cn_check: true` turns TLS on.** Leave it out on a plain (non-TLS) broker, or the board never connects. All examples are updated.
- **Optionally protect OTA and the web page**: the examples leave the `ota:` password out so a first build needs no extra secret; the commented examples show it as a ready line (`password: !secret ota_password`). Set it if other people use your network. If you enable `web_server:`, give it a login. The ESP-NOW repeater example does this for the node page (digest login), see [examples/repeater-espnow](examples/repeater-espnow/README.md).

Receiving wM-Bus frames from meters that are not yours is technically unavoidable (the radio hears everything in range). `forward_meters` limits what is published; use it if you only want your own meters on the broker.
