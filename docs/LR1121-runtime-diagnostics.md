# LR1121 runtime diagnostics (2026-09-06)

[Polska wersja](LR1121-runtime-diagnostics_PL.md)

Diagnostic-only change; modulation, IRQ masks, BUSY timeout policy and RX restart
decisions are unchanged. Other radio drivers return no runtime diagnostic.

Every 60 seconds the main task publishes a JSON snapshot (QoS 1, not retained) to
`<diagnostic_topic>/radio_runtime`, from `diagnostic_mode: low` upward. The same
snapshot is written to the log **only at `diagnostic_mode: dev`**: a register and
counter dump once a minute is bench instrumentation, and on a working node it
says nothing the summary does not.

Counters are cumulative since device boot. Record the topic during a test -
nothing is retained, so a snapshot you did not capture as it arrived is gone.
`uptime_ms` restarts on reboot and wraps after about
49 days.

- `busy_timeouts`: failed waits, including boot and direct-read waits.
- `status_samples`, `cmd_fail_observations`, `cmd_perr_observations`: sampled Stat1
  values, NOT unique failed commands. Consecutive transactions can report the same
  previous-command result. Invalid SPI responses can also look like CMD_FAIL.
- `stat1`, `stat2`, `chip_mode`, `reset_status`: last command/direct-read snapshot,
  not a live claim about the mode at publication time. ResetStatus is sticky;
  this patch does not clear it and cannot count separate resets.
- `irq_samples`, `rx_done_observations`, `timeout_observations`,
  `len_error_observations`, `read_without_rx_done`, `last_irq`: sampled before
  buffer reads. Repeated reads of an uncleared IRQ latch may be counted repeatedly.
  These are not preamble counts or numbers of on-air emissions.
- `packet_samples`, `packet_received_observations`, `packet_abort_observations`,
  `packet_length`, `packet_flags`: GetPacketStatus values already read by the
  existing RSSI path. Packet flags are raw; bit 1 = received, bit 2 = aborted.

Cross-task fields are atomic, but the complete JSON is not a single instantaneous
snapshot. Reading IRQ adds one direct SPI read and a BUSY wait per buffer-load
attempt. This is not a zero-overhead change: verify nominal reception before
comparing weak-signal results. No extra IRQ sources are enabled.

Sources: LR1121 User Manual rev 2.2, pp. 27-31, 38-39, 51, 80;
Semtech SWDR001 2.4.1, lr11xx_system.c and lr11xx_radio.c.

Known separate S1 issue: the existing SYNC_WORD_VALID constant incorrectly uses
bit 2 (TX_DONE); the documented bit is 5. Intentionally not switched here: enabling
the real early interrupt without fixing the receive dispatcher could abort packets.
This does not affect the T1 IRQ mask used in the attenuation experiment.

## Bounded FIFO and rejection samples

With summary diagnostics enabled, LR1121 also publishes these QoS 1 messages
(not retained - they were bench instruments, and a broker should not keep them):

- `<diagnostic_topic>/lr_pipeline`: cumulative main-task conversion counters,
  every 60 seconds. `converted = valid + decode_failed + length_failed +
  crc_failed + other_failed`. Counts precede the post-parse listen-mode filter.
  They do NOT include packets rejected in the receiver task before conversion.
  Compare against the existing summary `rx_path` counters for early rejection.
- `.../lr_fifo/0` through `.../lr_fifo/7`: rolling eight samples of the RX
  buffer, taken at most once per five seconds, regardless of eventual success or
  failure. A two-element, nonblocking FreeRTOS queue transfers copies from RX to
  main. If full, a sample is lost, never a received packet.
  These bytes may include noise after a short actual telegram due to fixed-length
  reception; do not count invalid trailing symbols as telegram corruption.

  Since 2026-09-22 the sample is **the whole 255-byte buffer read from offset 0**
  (`ReadBuffer8(0, 255)`, UM 2.2 p.35; RX RAM is addressable outside sleep, p.88),
  not the packet-sized read the decoder consumes. `fifo_dump` is 1 for such a
  sample and `raw_length` is 255. The reason: the packet-sized read takes
  `payload_len` bytes as reported by `GetRxBufferStatus`, and that is the length
  the packet engine was *declared* to expect - so it can never show whether the
  engine kept writing past it. With `payload_length` set below 255, the bytes
  past it are the only place that question can be answered. Cost is one extra
  255-byte SPI read after `RX_DONE`, at most once per five seconds; the decoder
  still receives the packet-sized read, unchanged.

  `probe` carries a **read-only** sample of four undocumented register
  addresses, taken right after `RX_DONE` in the same pass. `F20384` and
  `F20368` are the position-counter / end-of-packet pair that Semtech's own
  Sidewalk driver polls and writes on LR11xx with no named macro in the public
  SDK; `F30028` and `F30030` are the Rx FIFO base address and size registers
  documented for the LR20xx successor in the LR2021 datasheet rev 2.2, Tables
  5-2 and 5-3. Whether LR1121 has anything wired up at those addresses is the
  open question these values exist to answer. **Nothing is written to any of
  them.** Values are only interpretable against the `Register probe baseline
  (pre-RX, read-only)` line logged once at boot, taken after the radio is
  configured but before RX is ever armed - at that moment nothing has been
  received, so a position counter cannot legitimately hold a byte count. A
  value that never moves off the baseline, or reads as all-zero or all-ones,
  is a result and not a malfunction.

  That baseline is published to its own topic (not retained),
  `<diagnostic_topic>/probe_baseline`, and logged once from the main task as
  `Register probe baseline (pre-RX, read-only)`. Two reasons it is not simply
  logged from `setup()` and not folded into `radio_runtime`: component
  `setup()` runs before WiFi and the API are up, so a line emitted there never
  reaches `esphome logs` (restarting with the log attached does not help), and
  appending the four values to the `radio_runtime` JSON pushed that line past
  the logger's buffer - the log then printed the object cut off mid-key while
  MQTT still carried all of it. A diagnostic silently truncated in one of its
  two outputs is worse than one split across two lines.

  `packet_start` and `packet_len` carry the `GetRxBufferStatus` values for that
  capture, so the dump can be split into the declared packet
  (`[packet_start, packet_start + packet_len)`) and everything outside it.
  Without them the split would have to be assumed from the configured
  `payload_length`, which is exactly the assumption this dump exists to test.
  Bytes outside that range are not a decoded telegram and are not claimed to be
  one - they are whatever the RX RAM held at read time, which may be this
  capture, a previous one, or noise.

  Consequence for `verify`: the double-read comparison still covers only
  `payload_len` bytes, so `differing_bytes` and `first_difference` are indices
  into the packet-sized read, **not** into the 255-byte dump they are published
  next to. The dump is not the buffer that was compared.
- `.../lr_drop/0` through `.../lr_drop/7`: rolling eight failed conversions,
  at most once per five seconds. Includes actual parser reason/stage, lengths,
  3-of-6 symbol statistics and its raw input (up to 256 bytes). This input may
  already be trimmed by the receiver task; it is not necessarily the full FIFO.

No radio settings, restart policy or decoder decisions are changed. Sampling
adds bounded CPU/memory/MQTT overhead; verify nominal control reception.
Existing `diagnostic_publish_raw` and verbose logging need not be enabled.
Raw FIFO and drop sampling run independently: do not pair them by slot number.
Use boot ID and capture/wakeup uptime to correlate. The slot numbers repeat
every eight samples and nothing is retained, so subscribe before the test and
always filter by boot ID and test time. Record all these topics as well as `radio_runtime`,
the existing diagnostic summary and receiver JSON. No data is published while
MQTT is disconnected; these samples are not a durable recorder.

## Early RX outcomes and optional SPI verification

`lr_pipeline` now contains terminal RX-task counters since boot:
`rx_entered`, `rx_queued`, `rx_queue_failed`, `rx_preamble_failed`,
`rx_weak_probe_aborted`, `rx_size_failed`, `rx_payload_failed`, `rx_s1_failed`.
At rest, entered equals the sum of all seven outcomes. During reception atomic
snapshots may differ by an in-flight attempt. These count IRQ-woken receive
attempts, not unique radio captures. Queue-to-conversion discrepancies can also
include pending queue entries and listen-mode filtering. Existing wakeup uptime
on dropped packets and capture uptime on FIFO samples remain the correlation
keys; no exact one-to-one identity between independently sampled records is claimed.

To run the intrusive buffer experiment on LR1121 only:

```yaml
wmbus_radio:
  lr1121_verify_buffer: true
```

Default is false. For at most one sampled capture per 5 seconds, after RX_DONE:
SetStandby(XOSC), wait BUSY, verify command success and standby mode, obtain
buffer offset/length, read the FIFO twice at the same address, verify offset,
length and mode again. No ClearRxBuffer or reset is issued. The original first
copy continues through the unchanged decoder; the second copy is only compared.
Existing restart_rx re-arms reception afterwards. This can interrupt a following
packet and is NOT a passive sensitivity measurement. UM 2.2 pp.16,35,88 documents
standby and addressable RX RAM accessible outside sleep.

FIFO sample fields: `fifo_dump` = 1 when `raw` is the whole 255-byte buffer read
from offset 0, 0 when it is the packet-sized read (see above); `packet_start`
and `packet_len` locate the declared packet inside that dump. `verify` = 0
disabled/not requested, 1 inconclusive (BUSY, command status, mode or pointer
checks did not pass), 2 identical, 3 different. `differing_bytes` counts unequal
bytes; `first_difference` is a zero-based byte offset, or 255 when none.

The `verify` fields describe **the packet-sized read of `payload_len` bytes**,
which is not the same span as the `raw` dump published beside them, and is
shorter than 255 whenever `payload_length` is configured below 255. Equality
does not prove correct RF demodulation, and a stable deterministic SPI error is
not excluded. A mismatch under validated standby is evidence to investigate the
read path, not automatic proof of bad RF reception.

## Writing the expected packet length (bench experiment, default off)

```yaml
wmbus_radio:
  lr1121_expected_len_override: 64   # 0 = off, the default
```

Non-zero writes that value into bits `[31:20]` of `0x00F20368` after every
`SetStandby(XOSC)` and before `SetRx`, overriding the length `SetPacketParams`
declared. **This is the first write to an undocumented register in this
component.** It is bench work, not a supported configuration, and it logs one
loud warning when it engages.

What makes the address usable rather than a guess, measured 2026-09-22: the
field read 255 with the radio configured for 255 and *nothing yet received*,
and 64 after `payload_length` was changed to 64. The pre-RX baseline is what
separates "mirrors `SetPacketParams`" from "holds the last packet's length" -
in fixed-length mode those two are otherwise always equal. The field is
12-bit, so it can express up to 4095, past the 8-bit `pld_len_in_bytes` of the
public API.

The write is gated on the radio firmware version and refuses, with a warning,
on anything other than the image it was verified against (`0x0101` here). An
undocumented register is a property of a firmware image, not a promise.

**Reading the result:** `packet_len` in the FIFO samples comes from
`GetRxBufferStatus`. If it follows the override rather than the configured
`payload_length`, the engine obeys the register and the write is effective.
If it keeps reporting the configured value, the register is not the control
path from this direction - which closes the question rather than failing.

Start in the safe direction, override *below* the configured length: the
buffer is 255 bytes and lowering the expectation cannot overrun it. Raising it
past 255 is a separate step and needs a transmitter that actually sends more
than 255 raw bytes; the longest telegram in normal field traffic here is 245.

## Sampling the position counter mid-frame (bench experiment, default off)

```yaml
wmbus_radio:
  lr1121_sync_probe: true
```

Adds `SYNC_WORD_VALID` to the T1/C1 interrupt mask so the receiver task wakes
while a frame is still arriving, reads `0x00F20384` bits `[27:16]` once,
clears **only** that latch, and returns. Results go to
`<diagnostic_topic>/sync_probe` every 60 s: `sync_wakes`, `ptr_last`,
`ptr_max`.

The wake is **absorbed inside the driver**: after sampling, it polls the
counter until `RX_DONE` and then falls through into the ordinary capture, so
the caller never learns an early wake happened. Returning instead is what the
first two attempts did, and it disabled reception both times -
`receive_frame()` opens every attempt with `restart_rx()`, i.e.
`SetStandby(XOSC)` + `SetRx`, which aborts the frame still on air. Measured:
59 sync wakes per minute, zero captures. `sync_polls` counts the readings taken
inside that window and `sync_timeouts` the frames where `RX_DONE` never arrived
within the frame's air time plus 50 ms.

Clearing the sync-word bit is also load-bearing. DIO1 stays asserted while any unmasked
interrupt stands and the pin is read on a rising edge, so an uncleared
sync-word latch holds the line high and `RX_DONE` never produces an edge at
all. The first version of this probe deliberately cleared nothing, to "stay
out of the way", and measured 59 sync wakes with zero captures - it had
silently disabled reception. Only bit 5 is cleared; `RX_DONE` and the error
bits must survive. If the packet happens to finish during the sample, the
handler re-reads `GetRxBufferStatus` and continues into a normal capture
rather than waiting for an edge that has already passed.

Why it exists: a frame longer than the 255-byte buffer wraps, and the bytes it
overwrites are gone - after `RX_DONE` the start of such a frame no longer
exists anywhere. Capturing one therefore means draining during reception, and
that needs to know how much has arrived. `0x00F20384` reads 0 after `RX_DONE`;
whether it is live mid-frame is exactly what this measures. A counter that
stays 0 here means a drain would have to be timed off the bit rate instead.

**`IRQ_SYNC_WORD_VALID` was wrong until 2026-09-22** - it said bit 2, which is
`TX_DONE`. In a receive-only driver that bit never fires, so the S1 mask that
included it carried a dead bit and the S1 "sync matched but no packet"
diagnostic behind it never ran. The constant is now the documented bit 5 (UM
2.2 pp.37-39). Correcting the number changes nothing for S1 in practice, but
*enabling* the real interrupt there would: the S1 path clears the entire IRQ
latch, which would take `RX_DONE` with it. S1 therefore no longer requests the
bit at all, and enabling it for S1 needs dispatcher work first.

**Known side effect while this is on:** each early wake runs the normal receive
path, finds no packet and counts as `rx_preamble_failed`. That counter is
inflated for as long as the probe is enabled and should not be compared against
runs without it.

## Draining the frame while it arrives (bench experiment, default off)

```yaml
wmbus_radio:
  lr1121_sync_probe: true     # required - no early wake, no window to read in
  lr1121_drain: true
```

Inside the sync-word window the driver now also copies the frame out as it
lands. `0x00F20384` counts bytes received in the current frame **absolutely** -
it reached 325 on a 326-byte frame, so it does not wrap at 256 even though the
buffer does. Byte *k* therefore sits at buffer position *k* mod 256 and stays
readable until byte *k*+256 arrives. Measured margin at 100 kb/s: one poll
every ~2.8 ms against a 20.5 ms overwrite deadline, about 9 polls per frame.

Reads stop at the ring seam so a single `ReadBuffer8` never straddles the wrap,
and its length field is 8-bit so no read exceeds 255 bytes. Capture is capped
at 512 bytes.

**Test it on a frame that does not wrap first.** With an expected length at or
below 255 the ordinary post-`RX_DONE` read is a complete, correct copy of the
same bytes, so the drain is checked against it on-device, byte for byte, with
no offline reconstruction:

| field | meaning |
|---|---|
| `drain_frames` | frames where a comparison was possible |
| `drain_match` / `drain_mismatch` | how those comparisons came out |
| `drain_bytes_last` | bytes drained from the last frame |
| `drain_diff_last`, `drain_first_diff` | size and position of the last disagreement |
| `drain_served` | frames the decoder was fed from the drain instead of the buffer read |

`sync_probe` was schema 2 when `drain_served` was added and is schema 3 now;
see the auto-length section below.

Once a frame wraps that reference is destroyed - the post-`RX_DONE` read no
longer contains the start of the frame - and these comparison counters stop
meaning anything. That is exactly why the drain is proven below 255 before it
is trusted above it.

Above 255 the drain is published instead of compared: the last completed
drain goes to `<diagnostic_topic>/lr_drain` as `{seq, len, raw}` every 60 s,
because the post-`RX_DONE` read can no longer serve as a reference once the
start of the frame has been overwritten. Checking it means correlating those
bytes against a stream reconstructed outside the device. The snapshot and the
receiver task are not interlocked, so a sample taken while the next frame is
draining can tear; `seq` identifies the attempt, and a torn sample fails
correlation outright rather than producing plausible wrong bytes.

No automatic firmware deployment or experiment start is part of this change.
# Long-packet offset experiment (2026-09-22)

With `lr1121_drain: true`, `lr_drain` now uses schema 2 and includes a
bounded trace of ReadBuffer8 calls alongside the raw frame. Each row follows
`trace_fields`: `us`, `target`, `copied`, `packet_len`, `start`, `offset`, `size`.
`us` is elapsed time before the buffer read, relative to entering the drain
loop; `target` is the sampled absolute counter (or expected length for the
RX_DONE tail), `copied` is the number already drained. `packet_len` and `start`
come from GetRxBufferStatus just before draining that target. A split at the
ring boundary shares that status sample. At most 16 read calls are stored;
`trace_total` reports the total, so truncation is visible.

Addressing remains `copied % 256`. This experiment measures whether the start
pointer changes or is nonzero; it does not assume that adding it is correct.
The additional status transaction changes polling timing. If content improves,
that alone cannot distinguish a pointer issue from a write-visibility delay.
No trace row is added when there are no new bytes to copy.

The RX task transfers the raw bytes and trace together through a one-element
overwrite queue. Main publishes the latest complete sample; this is not an
archive of every frame. This also removes the previous unsynchronized shared
snapshot. Only completed captures are published.

Run with the existing 326-byte test frame, override 326, sync probe and drain
enabled. Confirm a new boot and `lr_drain.schema == 2`, export retained MQTT,
then correlate bytes at the recorded chunk boundaries. A zero `start` at all
observed reads weakens the proposed start-pointer correction. For wrapping
frames, do not use `drain_mismatch` as a correctness verdict.

**Outcome (2026-09-22/23).** `start` was zero in every recorded read, so the
start-pointer correction was dropped. With an insert/delete-aware alignment
rather than a byte-aligned correlation the drain is correct: a 326-byte frame
came back bit-exact, 2604/2604, through the wrap. The residual differences are
extra bits inserted by the *transmitter* - a software DCLK loop preempted about
every 1 ms - confirmed from the transmitter's own timestamps, not inferred from
the received stream. A byte-aligned comparison cannot tell one inserted bit
from a wrong read address: both collapse to chance at a single point.

## Deriving the length from the frame itself

```yaml
wmbus_radio:
  lr1121_sync_probe: true     # required - no early wake, no window
  lr1121_drain: true          # required - the header has to be in hand to read
  lr1121_auto_length: true
```

Off by default, and the three options only work together: setting
`lr1121_auto_length` alone would arm the engine with a ceiling and never narrow
it, so the configuration is rejected rather than silently doing the wrong thing.
It is also refused alongside `lr1121_expected_len_override`, which does the same
job by the opposite means - pinning every capture to one length.

RX is armed with a ceiling of `DRAIN_CAP` bytes instead of a fixed length. As
soon as four bytes have been drained the driver reads the L-field out of them,
computes how many raw bytes the frame really occupies, and writes that into
`0x00F20368[31:20]` **while the frame is still arriving**, so `RX_DONE` fires at
the real end. That mid-reception write is how Semtech's own Sidewalk driver uses
this register, and it is the only way a fixed-length engine can stop at a length
it could not know when RX was armed.

The arithmetic is the same one the SX1262 has used since its AN1200.53 path was
written, moved to `frame_length.h` so there is one copy:
`expected_raw_len_t1()` (3-of-6, L-field from the first two raw bytes),
`expected_raw_len_c1()` (no coding, L at index 2 behind the mode-C indicator)
and `expected_raw_len_s1()` (Manchester, with the polarity search and the
tolerances measured at the sensitivity threshold).

Two rules the implementation keeps:

**It never shortens backwards.** A length is only written when it is greater
than what has already been drained. Telling the engine a packet ended before it
did is not recoverable.

**A failed derivation is never worse than not trying.** If no length can be read
by 48 drained bytes, `payload_length_` is written - exactly what the board
captures today without this path. Compare with the SX1262, where a failed
derivation runs to a 512-byte cap and costs 125 ms of deafness.

| field in `sync_probe` | meaning |
|---|---|
| `auto_len_resolved` | frames whose length came from their own L-field |
| `auto_len_fallback` | frames where it could not be read, so `payload_length` was used |
| `auto_len_last` | the last length derived, in raw bytes |

`sync_probe` is schema 3 since these were added.

**Confirmed on hardware 2026-09-24, in all three modes.** The open question was
whether the LR1121 honours a write to this register *during* reception - Sidewalk
uses it that way, but that was inference from source. It does: `drain_bytes_last`
comes back as the derived length rather than the ceiling RX was armed with.

One L=0xBE telegram, three modes, three different lengths, each computed from the
same L-field by that mode's own arithmetic:

| mode | `auto_len_last` | resolved / fallback | note |
|---|---:|---|---|
| T1 | **326** | 57 / 1 | 3-of-6, x1.5 |
| S1 | **434** | **44 / 0** | Manchester, x2 |
| C1 | **219** | 59 / 59 | no coding, + 2 indicator bytes |

None of those numbers appears in the source, so an implementation returning a
constant or reusing a previous answer could not have produced them.

S1 was tested first on purpose. `transceiver_sx1262.cpp` records that on *that*
chip every S1 capture ends at `buffer_cap` because the frame does not begin at
chip 0 of the buffer, so a length is never derived. On the LR1121 it is derived
44 times out of 44 - the assumption holds here. S1 also resolves better than T1
for the same reason it decodes better: derivation needs four clean leading bytes,
and at 32768 b/s a transmitter inserts bits far less often.

The even split in C1 is not an auto-length defect. It is the C-mode sync cycling
false-syncing on the mode-C indicator, so those captures start at `FF 44` instead
of `54 CD` and no length can be read from them. They fall back, which is the
designed behaviour, not a failure of it.

**Still not shown:** that it adapts to *varying* lengths. The bench transmits one
frame repeatedly, so three modes give three numbers but not three lengths within
one mode. Real traffic with mixed meters is what would settle that - watch
`auto_len_last` change between frames.

## S1: the probe runs there too - and it turned out to be a fix

Since 2026-09-23 the sync-word probe and the drain also run in `listen_mode: s1`.
**Corrected 2026-09-23, same day:** this was written as a measurement and it is
not one - it changes S1 capture behaviour, and S1 now receives.

The IRQ mask sets `SYNC_WORD_VALID` whenever `lr1121_sync_probe` is on,
*regardless of mode*, while the branch that absorbs that wake used to exclude
S1. So in S1 with the probe on, the driver woke on the sync word, read
`GetRxBufferStatus`, saw `payload_len == 0`, returned a failed attempt - and
`receive_frame()` answered with `restart_rx()`, aborting the frame that was
still arriving. That is the same trap documented above for T1, latent in S1
because nobody ran S1 with the probe until now.

Measured immediately after the change: `converted` 323, `valid` 311 (96%),
`decode_failed` 0, `rx_preamble_failed` 0, `sync_timeouts` 0, and
**`drain_match` 323 against `drain_mismatch` 0** - the on-device self-check,
which is authoritative here because a 255-byte frame does not wrap the ring.

`ptr_max` still answers the original question when S1 fails for another reason:
0 means the modem hears nothing, advancing means bytes are landing and only the
packet engine's end condition is absent.

Two details this depends on. The air-time deadline uses the bitrate S1 actually
runs at: `bitrate_bps_` still holds the T-mode default of 100000 while S1 runs at
32768, and the unadjusted value gives a deadline three times too short, turning
every frame into a timeout that means nothing. And the timeout path clears all
IRQs in S1, because DIO1 stays asserted while any latch is set and the pin is
read on the rising edge - the non-probe S1 path clears them for that reason and
returning early would otherwise skip it.

`DRAIN_CAP` is 640, which covers the longest frame any mode can produce: S1 is
Manchester, so its maximum is 2 x 290 = 580 raw bytes, against 435 for T1 and
292 for C1.

## Feeding the drained frame to the decoder

Once a drain completes, the decoder is fed from it instead of from the
post-`RX_DONE` buffer read. Past 255 bytes that read cannot be the frame:
the buffer is a ring, `GetRxBufferStatus` reports `expected mod 256` (70 on a
326-byte frame), and the start of the telegram has been overwritten by its own
tail. The drained copy is the only complete one.

The substitution happens only when the drain reached the declared length. A
drain that ran short - past the 512-byte cap, or polls that fell behind the
write pointer - leaves the ordinary read in place, because a fragment handed to
the decoder would read as a corrupt frame rather than as a failed drain.

It is applied **after** the on-device self-check and the `lr1121_verify_buffer`
comparison, both of which read `rx_buffer_` over SPI. Substituting earlier would
make `drain_match` compare the drain against itself - an instrument that reports
success by construction is worse than none.

Below 255 bytes nothing observable changes: the two are the same bytes, measured
57/57 byte-for-byte before this was enabled. The substitution is not conditional
on length, so the path long frames take is the one short frames exercise daily.

`lr_fifo` still samples the chip's buffer, which is no longer what the decoder
receives once a drain has been served - compare it against `lr_drain`, not
against what was decoded.

Wire layouts were checked against Semtech's reference implementation:
[GetRxBufferStatus](https://github.com/Lora-net/SWDR001/blob/master/src/lr11xx_radio.c)
and [ReadBuffer8](https://github.com/Lora-net/SWDR001/blob/master/src/lr11xx_regmem.c).
