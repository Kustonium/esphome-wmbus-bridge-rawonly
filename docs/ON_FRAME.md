# on_frame automation

[Polska wersja](ON_FRAME_PL.md)

`on_frame` is an optional ESPHome automation trigger that fires every time the radio
receives a valid, CRC-correct wMBus frame. It gives you direct access to the raw frame
bytes inside a `lambda:` block before (or instead of) the built-in MQTT publish.

## When to use

Use `on_frame` when you need custom processing that the built-in MQTT forwarding does
not cover — for example sending frames over a TCP socket, filtering by meter ID in
firmware, or logging specific fields locally.

For most setups the built-in `topic_name` / `telegram_topic` forwarding is enough and
`on_frame` is not needed.

## Basic syntax

```yaml
wmbus_radio:
  radio_type: SX1276
  # ... other options ...
  on_frame:
    - then:
        - lambda: |-
            std::string hex = frame->as_hex();
            ESP_LOGI("on_frame", "hex: %s  RSSI: %d dBm", hex.c_str(), (int)frame->rssi());
```

## Frame object methods

Inside the `lambda:` block the variable `frame` is a pointer to the received frame.

| Method | Return type | Description |
|---|---|---|
| `frame->as_hex()` | `std::string` | Full frame as uppercase hex string — ready to publish to MQTT or pass to wmbusmeters |
| `frame->as_raw()` | `std::vector<uint8_t>` | Raw frame bytes |
| `frame->as_rtlwmbus()` | `std::string` | Frame in rtl-wmbus text format |
| `frame->rssi()` | `int8_t` | RSSI in dBm at the time the frame was received |
| `frame->link_mode()` | `LinkMode` | `LISTEN_MODE_T1`, `LISTEN_MODE_C1`, `LISTEN_MODE_S1` |
| `frame->format()` | `std::string` | Frame format string, e.g. `"T1 A"` |
| `frame->try_get_meter_id(uint32_t &id)` | `bool` | Extract meter ID from the frame; returns `false` if extraction fails |
| `frame->mark_as_handled()` | `void` | Count the frame as handled (the log says "handled by N handlers"); does **not** stop the MQTT publish |

## mark_as_handled

`mark_as_handled: true` in the trigger block only counts the frame as handled: the
log line after the triggers reads "Telegram handled by N handlers" instead of
"not handled by any handler". It does **not** suppress the built-in MQTT publish -
that publish runs before the `on_frame` triggers, so by the time your lambda sees
the frame it has already gone out.

To stop the built-in publish:

- `repeater: true` - no MQTT at all; frames leave only through `on_frame` (a board
  that hands them on, e.g. over ESP-NOW, and may have no `mqtt:` block);
- `forward_meters:` - publish only the listed meters.

```yaml
on_frame:
  - mark_as_handled: true
    then:
      - lambda: |-
          // the frame is counted as handled; MQTT publish already ran
          std::string hex = frame->as_hex();
```

Earlier versions of this page said `mark_as_handled` suppresses the MQTT publish. The code never did that; the page was wrong (corrected 2026-11-17).

## Filtering by meter ID

```yaml
on_frame:
  - then:
      - lambda: |-
          uint32_t id = 0;
          if (!frame->try_get_meter_id(id)) return;
          if (id != 0x12345678) return;
          ESP_LOGI("on_frame", "target meter: %s  RSSI: %d", frame->as_hex().c_str(), (int)frame->rssi());
```

## Sending over TCP socket (socket_transmitter)

If the `socket_transmitter` component is loaded, you can use the
`wmbus_radio.send_frame_with_socket` action:

```yaml
on_frame:
  - wmbus_radio.send_frame_with_socket:
      id: my_socket
      format: hex       # hex | raw | rtlwmbus
```

## Multiple triggers

Multiple `on_frame` triggers are supported. Each fires independently for every frame.

```yaml
on_frame:
  - then:
      - lambda: |-
          ESP_LOGI("on_frame", "trigger 1: %s", frame->as_hex().c_str());
  - mark_as_handled: true
    then:
      - lambda: |-
          ESP_LOGI("on_frame", "trigger 2 (handled)");
```

## Notes

- `on_frame` fires only for frames that passed CRC validation. Dropped/truncated
  packets do not trigger it.
- The lambda runs in the ESPHome main `loop()` task, not in the radio receiver task.
- `frame` pointer is valid only for the duration of the lambda call. Do not store it.
