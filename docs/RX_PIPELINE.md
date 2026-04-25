# RX pipeline and frame qualification

This project is a **RAW-only bridge**, but `RAW` here does not mean “every byte blob from the radio”.

The component publishes only telegrams that passed the internal wM-Bus frame checks. Failed candidates can still be counted and reported by diagnostics, but they are not forwarded on `telegram_topic`.

## Processing model

```text
radio IRQ
  -> read PHY bytes from SX1262/SX1276
  -> build a packet candidate
  -> detect link mode hint: T1 or C1
  -> calculate expected candidate length
  -> parse and validate the candidate
  -> remove DLL CRC bytes
  -> publish validated telegram HEX to MQTT
```

## T1 path

T1 is 3-out-of-6 encoded. The receiver does not trust the first 3 bytes blindly.

Current logic:

- read the first 3 bytes,
- for T1, read an extended probe up to `WMBUS_T1_LEN_PROBE_BYTES` before calculating the length,
- decode the T1 3-out-of-6 prefix to obtain the L-field,
- read the expected remaining bytes,
- decode the full T1 payload,
- validate the L-field and DLL CRC blocks,
- drop the candidate if any stage fails.

Common T1 reject stages:

- `t1_decode3of6` — invalid 3-out-of-6 symbols,
- `t1_l_field` — invalid decoded L-field,
- `t1_length_check` — candidate shorter than expected,
- `dll_crc_first`, `dll_crc_mid`, `dll_crc_final` — DLL CRC failed.

## C1 path

C1 starts with `0x54` and uses a second sync byte variant. The radios cycle the second sync byte in C1/BOTH modes, because real C1 traffic may use more than one variant.

Current logic:

- detect C1 by the first byte `0x54`,
- verify the C1 block preamble,
- remove the two C-mode leading bytes,
- use the L-field and frame format to calculate expected size,
- validate DLL CRC,
- drop the candidate if any stage fails.

Common C1 reject stages:

- `c1_precheck`,
- `c1_preamble`,
- `c1_l_field`,
- `c1_length_check`,
- `dll_crc_*`.

## What `telegram_topic` publishes

`telegram_topic` publishes `frame->as_hex()` for successfully validated frames.

That means:

- T1 was decoded from 3-out-of-6,
- C1 was normalized by removing the C-mode leading bytes,
- DLL CRC bytes were validated and stripped,
- the payload is still not meter-decoded.

So it is “RAW-only” in the sense of **no meter decoding on ESP**, not in the sense of forwarding arbitrary radio garbage.

## Diagnostics versus forwarding

Diagnostics may count or optionally publish failed candidates:

- `payload_size_unknown`,
- `false_start_like`,
- `preamble_read_failed`,
- `t1_decode3of6`,
- `dll_crc_failed`,
- `truncated`.

These diagnostics are useful for RF and receiver analysis, but they are not successful telegrams.

Rule of thumb:

```text
candidate != valid telegram
raw diagnostic blob != published telegram_topic payload
```
