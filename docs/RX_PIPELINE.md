# RX pipeline and frame qualification

This project is a **RAW-only bridge**, but `RAW` here does not mean “every byte blob from the radio”.

The component publishes only telegrams that passed the internal wM-Bus frame checks. Failed candidates can still be counted and reported by diagnostics, but they are not forwarded on `telegram_topic`.

## Processing model

```text
radio IRQ
  -> read PHY bytes from CC1101/SX1276/SX1262/LR1121
  -> build a packet candidate
  -> detect link mode hint: T1, C1 or forced S1
  -> calculate expected candidate length
  -> parse and validate the candidate
  -> remove DLL CRC bytes
  -> publish validated telegram HEX and structured RX metadata to MQTT
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

## S1 path

S1 is a dedicated experimental receive path. It is selected explicitly with `listen_mode: s1` and does not participate in `both`.

Current logic:

- configure the radio for the S-mode / Manchester RF profile,
- force the packet candidate to `LinkMode::S1`,
- read raw bytes after S1 sync,
- try Manchester decoding,
- validate the L-field and DLL CRC blocks,
- publish the validated telegram HEX to MQTT,
- drop the candidate if Manchester, length or CRC validation fails.

Common S1 reject stages:

- `s1_precheck` — candidate too short,
- `s1_manchester` — Manchester decoding failed,
- `s1_l_field` — invalid decoded L-field,
- `s1_length_check` — candidate shorter than expected,
- `dll_crc_*` — DLL CRC failed.

`listen_mode: both` remains T1/C1 only. S1 must be selected explicitly and uses its own RF profile and default frequency.

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
- S1 was decoded from Manchester coding,
- C1 was normalized by removing the C-mode leading bytes,
- DLL CRC bytes were validated and stripped,
- the payload is still not meter-decoded.

So it is “RAW-only” in the sense of **no meter decoding on ESP**, not in the sense of forwarding arbitrary radio garbage.

## What the companion `/rx` topic publishes

For every validated, whitelist-eligible frame published on `telegram_topic`,
the component also publishes schema-1 JSON on `wmbus/<topic_name>/rx`. It
identifies the ESP boot and source-wide receive sequence, carries the receiver
task wake time after IRQ, meter ID, link mode, measured RSSI when available,
and the CRC32 and length of the final normalized frame.

This metadata is an observation of the ESP receive path, not a decoded meter
reading. The wake timestamp is deliberately named `rx_task_wakeup_us`: radio
drivers signal the task at different receive stages, so it must not be treated
as an exact on-air start or exact `RX_DONE` time.

`received_at` is added when the board's clock is set: an ISO-8601 UTC stamp with
milliseconds, for example `2026-08-24T06:41:12.481Z`.

It is the moment the frame was **received**, not the moment it was published.
The two differ - the frame is captured in the receiver task and reaches MQTT a
little later - so the value is computed backwards from `rx_task_wakeup_us`,
which is monotonic since boot. Stamping publish time would quietly relabel the
frame, which is precisely what a timestamp is supposed to prevent.

The field is **absent, not null, when the clock has not been set yet**. That is
not a rare edge: after a restart the radio receives normally for the seconds or
minutes SNTP needs to answer, and a frame from that window must not carry 1970
or an uptime dressed up as a date. A consumer that never sees the key cannot
mistake a placeholder for a measurement.

Adding an optional field does not change the schema version: a reader written
against schema 1 keeps working, and one that wants the timestamp checks whether
the key is there.

## Diagnostics versus forwarding

Diagnostics may count or optionally publish failed candidates:

- `payload_size_unknown`,
- `false_start_like`,
- `preamble_read_failed`,
- `t1_decode3of6`,
- `s1_manchester`,
- `dll_crc_failed`,
- `truncated`.

These diagnostics are useful for RF and receiver analysis, but they are not successful telegrams.

Rule of thumb:

```text
candidate != valid telegram
raw diagnostic blob != published telegram_topic payload
```
