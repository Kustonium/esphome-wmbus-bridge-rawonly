# Release notes

[Polska wersja](RELEASE_NOTES_PL.md)

## Feature: MQTT store-and-forward outbox with QoS and buffer controls

- **New option `mqtt_buffer_size` (default `0`, off).** When the broker goes away, decoded messages are held in RAM and published on reconnect instead of being dropped. Companion controls for QoS and per-meter priority ship with it, plus a runtime capacity number and a QoS select.
- **Use `auto` rather than a fixed number, and not only for sizing.** With `auto` the ceiling tracks free heap, so reaching it coincides with real memory pressure and the priority eviction logic is what defends the node. With a fixed capacity larger than the heap can back, the buffer never considers itself full, eviction never runs, and the only defence left is the RAM valve - which knows nothing about `buffer_priority`. `auto` therefore turns blind loss into chosen loss.
- **Verified on hardware by someone other than the author**, across both memory paths - PSRAM and internal heap - over more than three hours of induced broker outages: no watchdog, no reboot, no RX FIFO overflow, exact accounting in every run, and nothing that entered the buffer was lost.
- **Known behaviour, not a defect:** on a board without PSRAM the largest free heap block does not fully recover after a drain, although total free heap does. Observed floors stayed at roughly twice the fragmentation threshold across two outage cycles, so the valve refused on total free heap every time and never on fragmentation. Worth watching on boards with a smaller starting heap.
- Off by default, so this changes nothing until you ask for it.

---

## Feature: `min_preamble_bits` - one preamble gate for SX1262, SX1276 and LR1121

- **New option, and one rename.** `min_preamble_bits` exposes the gate on SX1262 and SX1276, where it was not configurable before. On LR1121 it replaces `preamble_detector`, which did the same thing under a chip-specific name - if you set that key, rename it. LR1121 is behind `lr1121_allow_experimental`, so few configurations are affected. Rejected on CC1101 with a reason: that chip has the same gate as PQT, but counted in quality steps rather than preamble bits.
- **This is not the transmit preamble length.** That is a separate `SetPacketParams` field and is not configurable from YAML. The old name invited the confusion.
- **For `listen_mode: t1` and `both`, 16 is the maximum and the validator now refuses more.** Measured on hardware: an SX1262 at 24 bits took 184 receiver triggers in two minutes and decoded zero frames. The detector waits for 24 bits of continuous preamble, the T1 preamble is shorter, so detection never completes and every frame flies past. 16 works, so the usable preamble is 16-23 bits, consistent with the 19 usually quoted for T-mode. A board configured that way is silently deaf while looking healthy, which is why it is refused at validation rather than discovered on a roof.
- `8` still works and is what upstream uses, but it costs about **16% of the meters heard** - reproduced on two antenna-matched board pairs and again day-over-day on two boards. **All of that is one location**: five boards in one block of flats. The direction has held under every method tried there; the size of the effect is a property of that RF environment and should not be read as a constant.
- C1 and S1 are left alone. Their preambles were not measured here, and guessing in the direction that silently disables a receiver is the wrong way to be wrong.

---

## Feature: receiver-trigger counter, and "no frames" split into two diagnoses

- **New counter `irq_fired`** in the diagnostic summary and in the log line. It counts the data interrupt itself, before any parsing, which makes it the only counter that separates "never heard anything" from "heard it and lost it downstream".
- **Read it next to `dropped`, never `drop_pct` alone.** `drop_pct` improves as reception gets worse: a frame the radio never attempted is not counted as dropped. Measured across five boards in one window, the board with the lowest `drop_pct` (8%) delivered half of what the board with the highest (21%) did, and the board with the worst conversion (`total / irq_fired` = 11%) heard the most meters of all.
- **`NO_DATA` now splits in two.** No triggers at all still means antenna, frequency or wiring. Triggers without frames is a different fault and now says so: something IS transmitting, but nothing passes the filter - check `listen_mode`, `min_preamble_bits` and the meter's own mode. That split diagnosed a deaf board in one minute during testing.

---

## Fix: the long-frame capture ran past the end of the frame, every time

- **`long_gfsk_packets` streaming now ends where the frame ends.** The T-mode capture had no way to learn a frame's length, so it ran to its 512-byte cap collecting post-frame noise - not occasionally, but on every capture, because both of the loop's early exits are unreachable in continuous RX: `RX_DONE` cannot fire while the code pushes the payload-end register ahead of the write pointer, and the silence check never trips because the demodulator keeps producing bytes out of noise. Verified on hardware: `captured=134 exit=t1_length`, where it used to be `captured=512 exit=buffer_cap`.
- The length comes from the frame's own L field, decoded from the first two raw bytes. When it cannot be decoded the capture now stops immediately instead of gathering a shorter window - without an L field nothing downstream can find the block boundaries, so every further byte is time spent deaf over a frame that will be discarded anyway. `diagnostic_mode: verbose` keeps the old window for people who are deliberately watching.
- **Still true, and now stated in the docs: `long_gfsk_packets` costs about 7 dB in weak signal.** Measured off/on/off on one board over three windows, with three untouched controls holding their RSSI percentile to the decibel. The capture ending in the right place did not remove that penalty, so the option remains a trade, not a free win. Check your longest decoded frame before enabling it prophylactically - below roughly 150 decoded bytes it buys nothing.

---

## Fix: `false_start_like` counted every false start twice on SX126x

- `payload_size_unknown` and `raw_drain_skipped_weak` fire on the same event on that path - the drain is skipped precisely because no length could be derived and the signal was weak - and the aggregate added both. Ten consecutive summaries from four boards had the two equal to the unit and the total exactly double.
- **Not cosmetic:** the diagnostic hint downstream triggers at 40 or more, so it was really firing at 20.
- Fixed by taking the larger of the pair rather than dropping one of them. On the SX1276 path they are not the same event - `raw_drain_skipped_weak` stays at zero while `probe_start_aborted` carries the count - and that case was already correct.

---

## Feature: CC1101 hints at a marginal SPI connection when a write needs a retry

- When `apply_radio_profile_()` needs a retry to land a register, or a register never holds its value at all, the existing `CC1101 profile write-back` line now gets a follow-up `CC1101 hint` explaining what that number likely means.
- Two tiers: a register that never held its value even after retries points at a persistent fault - reseat every connection, check for a short between adjacent pins, try a shorter cable. A register that needed a retry but landed points at something intermittent - a loose header pin, a marginal short, a cable too long for the clock speed.
- **Why "likely" and not "confirmed":** `chip_not_ready_count_` already rules out `CHIP_RDYn` as the cause of a retry - if a write is answered with the chip ready and the value still does not read back, the bus corrupted a byte in transit. Issue #22 traced one real case of this to an intermittent short between two jumper wires: `reg_retry_count_` went from double digits to zero the moment the short was fixed, with nothing else about the setup changed. One correlated report is a lead, not a proof, which is why the hint says "likely" rather than naming a cause.
- This is CC1101-only. The write-verify-retry mechanism it reads from only exists on that driver.

---

## Fix: FREQ and SYNC writes were not verified, and FREQ was the one that mattered

- The read-back verification added for the boot profile (`write_reg_verified_`) covered every register written from `apply_radio_profile_()` directly, but missed two callees that write registers of their own: `set_frequency_()` (`FREQ2/1/0`) and `set_sync_word_()` (`SYNC1/0`, both the normal and the S1 path).
- **Issue #22 caught it immediately.** With `spi_data_rate: 1MHz` in place, `FREQ1` landed but `FREQ2` and `FREQ0` stayed at their reset defaults - the radio was listening on 790.961 MHz instead of 868.950 MHz, with everything else in the profile now correct. The FREQ validation added earlier this same day is what made this visible instead of silent.
- All five registers now go through the same write-verify-retry path as the rest of the profile.

---

## Feature: `spi_data_rate`, and proof that the SPI bus was the problem

- **New option `spi_data_rate`** (all radios, default unchanged at 2 MHz). Sets the SPI clock for the radio device only, not for the whole bus. Range 100 kHz to 8 MHz.
- **Why it exists.** On the board in issue #22, `reg_write_retries=4` with `reg_write_failed=0`: every configuration register eventually took its value, but four of them needed a second attempt. That is a bus corrupting bytes, measured rather than guessed — and it happened on a healthy 3.3 V supply, with the module on jumper leads.
- **The failure is silent, which is the point.** A dropped bit in a register write leaves that register at its reset default. Nothing reports an error; the radio simply behaves as if it had been configured for a different data rate and deviation, and every frame fails its CRC three layers further down. Before the read-back verification landed, this was indistinguishable from a dead antenna.
- **Lower it before suspecting the part.** Raising it above 2 MHz is permitted but has no known benefit here — the RX FIFO drain is paced by the radio, not by the bus.
- **What it does not fix.** Register writes are repeated on mismatch, so they recover. The RX FIFO read cannot be repeated: a second read consumes bytes the first one already took. If the bus corrupts a byte there, the frame is lost and no amount of retrying will bring it back. That is the case for setting a clock the wiring can actually carry.

---

## Fix: CC1101 lost register writes when the chip was not ready

- **`CHIP_RDYn` is now honoured.** The CC1101 answers every header byte with a status byte whose bit 7 is `CHIP_RDYn`; TI SWRS061I 10.1 requires it to be low before the first `SCLK` edge. The driver received that byte on every transaction and discarded it. Writes and strobes now inspect it and repeat the transaction when the chip reported itself not ready - up to 5 attempts, 200 us apart.
- **Reads are counted, never repeated.** A second read of the RX FIFO would consume bytes the first one already took. Single-register reads set the counter too, because a read taken before `CHIP_RDYn` goes low returns `0xFF` rather than the register.
- **The reset sequence follows the datasheet.** `reset_cc1101_()` now waits for `CHIP_RDYn` before issuing `SRES` and again afterwards, and the post-reset settle went from 5 ms to 10 ms, matching both known working CC1101 drivers. Previously the strobe went out immediately after `CSn` fell, which is only legal if the caller instead honours `tsp,pd` from Table 22 - and that 150 us figure was measured on a CC1101EM reference board with a specific crystal, not on an arbitrary module.
- **The `0x54CD` sync cycle stays, for now.** It was removed and then restored the same day. The case for removing it is real - mode T and mode C share the sync word `0x543D`, and the `0x54CD` that follows in a mode C telegram arrives as *data*, which is why `packet.cpp` strips it as `WMBUS_MODE_C_SUFIX_LEN`. But the only capture behind that reasoning came from a board whose RF profile does not apply correctly, so it says nothing about what a healthy receiver sees, and the sibling drivers cycle for a documented reason. Settling this needs a measurement from a working CC1101.
- **`FREQ2/1/0` is validated.** The carrier registers were written and never read back, so a frequency word that failed to land left the radio tuned elsewhere while the self-check reported everything fine. This is the only part of the profile that depends on user YAML, and a silently wrong carrier is indistinguishable from a dead antenna in every other log line.
- **Every register of the boot profile is now written, read back, and rewritten on mismatch** (3 attempts). The self-check already reported that the profile was wrong, but only at the end and only as a list of final values - it could not say which write failed, whether repeating it helped, or how often. That distinction is the whole diagnosis: a value that a repeat fixes means the transport is corrupting bytes, while a register that keeps reverting to its reset default means no amount of retrying will help and the fault is in the part or its supply. Mismatches are logged, never fatal: a few registers may legitimately read back differently (reserved or read-only bits), and refusing to boot over that would be worse than the problem being measured.
- **New fields `chip_not_ready`, `reg_write_failed` and `reg_write_retries` in the `CC1101 debug status` line**, counting transactions the chip answered with `CHIP_RDYn` still high. A non-zero value points at supply voltage, wiring length or SPI clock - not at the decoder.
- **Why this surfaced.** A user running a CC1101 module at 2.932 V found that roughly two thirds of the configuration registers never took, and that inserting a delay after every register write made the problem disappear. On a well-supplied module the chip happens to be ready in time and nobody notices the missing check; a slower crystal start-up exposes it. Reported by @lente-cz, whose logs are the only hardware evidence this driver has.

---

## Feature: measured noise floor, and an opt-in threshold based on it

- The diagnostic summary gains `noise_floor_dbm` and `noise_floor_n`: the ambient RSSI of the channel, sampled while the receiver sits armed and idle, plus how many samples stand behind it. **Always on** - no option needed.
- Sampled only where a full 5 s hop elapsed with no interrupt, so nothing was being received. Reported as the MINIMUM of a 16-sample ring, not a mean: a sample taken while a neighbouring meter transmits reads high, and averaging would let that drag the floor up.
- New options `use_noise_floor_threshold` (default `false`) and `noise_floor_margin_db` (default 6). When enabled, the weak-start abort threshold becomes `floor + margin` instead of `recent_ok_rssi_avg - 12`.
- **Why this matters.** The existing threshold is derived from an average over SUCCESSFUL receptions, so aborting weak frames raises the average, which raises the threshold, which aborts more - a feedback loop with no external reference. The noise floor has no such loop: it is what the channel does when we are not receiving.
- **And it is portable.** A board with a FEM reads roughly 10 dB hotter than the same chip without one - measured on the bench 2026-08-25, two SX1262 boards, medians -59 vs -68 dBm and minima -79 vs -89. An absolute clamp like `[-96, -86] dBm` therefore means something physically different on each board, while "N dB above the floor" means the same everywhere.
- **The threshold is off by default on purpose.** No measurement of a real noise floor existed when this was written, so any margin would have been a guess. The measurement ships enabled precisely so the margin can be chosen from numbers instead.

---

## Feature: `/diag/config` retained configuration snapshot

- One JSON payload published retained on `wmbus/<topic_name>/diag/config` after the first MQTT connect, refreshed once per boot.
- Shape: `{"radio":"SX1276","lines":["  listen_mode: t1 (CHANGED, default: c1)", ...]}` - the exact same lines the boot log prints, marker included, so a reader can compare panel and log without translation.
- Marker vocabulary: `(default)`, `(CHANGED, default: X)`, `(set)` for fields without a default, `(required)`, and `(mode default: X)` for `frequency` when unset. The add-on's diagnostics panel parses the trailing marker for the badge color and prints the rest verbatim.
- Retained so a reader opening the panel long after boot still sees the configuration the board came up with. Not chunked - the whole snapshot fits in one MQTT publish for the current schema.
- Why: the boot log already carried every effective setting with default/changed markers, but only over the serial or `esphome logs` transport. Publishing the same text makes it visible from the add-on without asking for the YAML.

---

## Change: `sx1276_busy_ether_mode` now defaults to `normal`

- **The default changes from `adaptive` to `normal`.** `adaptive` and `aggressive` do not tune the receiver: they abort weak starts so the radio can keep up when it is overrunning. If it is not overrunning, that sensitivity is spent for nothing.
- Measured on a dense apartment block, 2026-08-23, with all four boards **in one physical spot**: under `adaptive` **no frame weaker than −84 dBm got through at all** and the board heard **27** meters; under `normal` frames arrived down to **−97 dBm** and it heard **53**. Normalised against the three control boards in the same window, their meter counts did not move (44→45, 33→33, 27→24) while the SX1276 went 27→53.
- The mechanism matches the code: the abort threshold in `should_abort_t1_probe_start_()` is clamped to `[-96, -86]` and `adaptive` adds +4 dB, which pins it to the clamp. The result is a hard sensitivity floor, not a gradual trade.
- Throughout that day `fifo_overrun`, `truncated`, `payload_read_failed` and `irq_timeout` were **zero** - the receiver was never overrunning, so the protection was buying nothing.
- **New diagnostic suggestion `CONSIDER_BUSY_ETHER_ADAPTIVE`**: the component now proposes `adaptive` only when overload is *measured* (`fifo_overrun > 0` or `truncated > 0`) together with real losses (`drop_pct >= 10`). A high `false_start_like` alone is explicitly not a reason - it counts noise triggers, and it sat near 60/min all day with zero overruns.
- `CHIP_SELECTION{,_PL}.md` and `TROUBLESHOOTING{,_PL}.md` §8 rewritten around that rule, including a warning that `drop_pct` improves by itself under `adaptive` because the frames it would have counted are no longer attempted. Judge the mode by per-meter counts instead.
- SX1276 examples no longer set `adaptive` actively; the commented ones annotate `# default: normal`.
- **Caveat, stated in the docs as well:** one board, one building, one evening. The mechanism is understood; the size of the effect elsewhere is not.

---

## Feature: `/rx` metadata carries the reception time

- The `wmbus/<topic_name>/rx` payload gains `received_at`, an ISO-8601 UTC stamp with milliseconds.
- It marks when the frame was **received**, not when it was published. The frame is captured in the receiver task and reaches MQTT later, so the value is computed backwards from the monotonic `rx_task_wakeup_us`. Publish time would relabel the frame - the exact failure a timestamp exists to prevent, and the one that matters most to anyone buffering frames while the broker is unreachable.
- **Absent, not null, when the clock is unset.** After a restart the radio receives normally for as long as SNTP takes to answer; a frame from that window must not carry 1970 or an uptime pretending to be a date. A reader that never sees the key cannot mistake a placeholder for a measurement.
- No schema bump: the field is additive and optional, so a consumer written against schema 1 is unaffected.
- Asked for on the forum, alongside a durable store-and-forward buffer. The timestamp is the half that is cheap and unambiguous; buffering is not, because a flash write lands on the timing-sensitive receive path.

---

## Feature: the boot log now states the whole configuration, and says what you changed

- Every radio now logs a **configuration report** at boot: each effective option on its own line, grouped into `[core] [pins] [<radio>] [output] [diagnostics]`, and marked `(default)`, `(CHANGED, default: X)`, `(set)` or `(required)`. Previously the log carried a handful of hand-picked checks, so any option outside that list was invisible and a misconfigured board still looked healthy.
- The report is generated at **compile time from the schema**, not restated in the driver, so a default in the log can never drift away from the default the component uses. Only options that apply to the selected radio are printed.
- **`rf_sw_pin` is now reported for SX1262.** Its absence is the same class of silent failure as `has_tcxo: false`: the radio initializes, the log looks healthy, and a XIAO ESP32-S3 + Wio-SX1262 runs roughly 30 dB deaf because the module never opens its antenna path. It is stated in both directions, so "not configured" is a positive statement rather than a missing line.
- **CC1101 had no sanity block at all** and now has one: the experimental gate plus `gdo0_pin`/`gdo2_pin`, so dual-IRQ wiring is confirmed instead of inferred.
- Coverage before this change was uneven — SX1276 logged one check, SX1262 four, LR1121 six, CC1101 none.
- Documented in `DIAGNOSTIC{,_PL}.md` with the marker table and the per-radio list.

---

## Docs: SX1262 examples with a TCXO now clear the device-error register

- All eight `SX1262` examples (Heltec V3, V4, V4-R8, XIAO) set `clear_device_errors_on_boot: true` and `publish_dev_err_after_clear: true` instead of listing them as optional extras. Every one of those boards declares `has_tcxo: true`, and on a TCXO board `XOSC_START_ERR` is set on every power-up as a matter of course: the chip tries its own crystal before DIO3 has been told to power the TCXO, and DIO3 is configured after reset.
- Left off, the flag is therefore always set and carries no information. Cleared once the reference is up - which is what the datasheet expects - it becomes a diagnostic: a flag that stays cleared was a power-on artefact, a flag that comes back after a clean clear is a reference that genuinely is not starting.
- `publish_dev_err_after_clear` sends the re-read state to MQTT. That is the only way to see it on a node receiving nothing, which is precisely the case where the error register is the last thing left to read - the situation the 2026-08-01 fix was about.
- Defaults in the schema are unchanged (`false` for both). This is a change to what the examples recommend, not to component behaviour.

---

## Fix: examples no longer reboot a standalone MQTT receiver every 15 minutes

- Every example YAML that pairs `mqtt:` with `api:` now sets `api.reboot_timeout: 0s`, with a comment explaining why. ESPHome's default is `15min`, and that timer restarts the board whenever no Native API *client* is connected - which is the normal state of a receiver that only publishes to MQTT.
- Measured, not deduced: the `/rx` metadata for the night of 2026-08-20/21 showed **51 distinct `boot_id` per board, median 900 s apart**, while Home Assistant had no ESPHome device added. After the change the same boards ran **14.1 h on one `boot_id`** with `seq` rising continuously.
- `api:` is kept, so Native API and `time: platform: homeassistant` still work; only the watchdog is off. `mqtt.reboot_timeout` is a separate mechanism for broker loss and is deliberately left alone.
- `TROUBLESHOOTING{,_PL}.md` gained a section for the symptom, including how to confirm it from `boot_id` and `seq` instead of guessing from telegram counts.

---

## Docs: the fourth radio, and the S1 answer, are now in the documentation

- `CHIP_SELECTION.md` covers all four supported radios (`CC1101`, `SX1276`, `SX1262`, `LR1121`) instead of two, and gained an **S1 section**: `SX1262` decodes S1 to about -82 dBm and fails at -85, while `SX1276` decoded the same real transmission in the same second at -99/-100 dBm. The practical rule — `SX1276` for S1, `SX1262` for T1 — was measured on 2026-08-01 and 2026-08-14 but had never reached the document people read before buying a board.
- `LR1121` is now present where a reader looks for it: `README.md`, `START_HERE`, `RADIO_OPTIONS_MINIMAL.md`, `README_FULL`, `TROUBLESHOOTING` (a new section for the three failures that look like dead hardware), and the `busy_ether_state: n/a` lists in the diagnostics docs. `radio_type` in `CONFIG_REFERENCE_MINIMAL.md` lists it as a valid value.
- The LR1121 example README states the measured weakest decode as **-114 dBm**, read from the `/api/esp-rx` export over a 14.1 h run on 2026-08-21, replacing the earlier -100 dBm. The same run produced 1401 frames at -105 dBm or below. Its "S1 untested" caveat is gone, because S1 was verified on 2026-08-19.
- `BENCHMARKS.md` states explicitly that `CC1101` and `LR1121` are **not** benchmarked: that comparison depends on both radios standing in the same place, and no such run exists for them.
- `RX_PIPELINE.md` documents the companion `/rx` topic, which only the English version had.
- `diagnostic_publish_suggestion` was the one schema option with no entry in the reference; it has one now.
- Both `CHIP_SELECTION` files now say what the numbers cannot support: RSSI is not comparable between boards (an external LNA/FEM reads 13-15 dB higher), and frame counts only compare boards standing in the same position.

---

## Feature: opt-in per-meter RSSI, and examples that state their defaults

- New option `publish_rssi` (**default `false`**). With it on, the board publishes the level of each forwarded meter's last frame as a retained integer to `wmbus/<topic_name>/rssi/<meter_id>`. Nothing changes for anyone who leaves it off — the topic simply never appears.
- Only frames with a real measurement are published. A frame the radio gave no level for is skipped rather than sent as a sentinel, so a consumer never has to guess whether `0`, `1` or `-127` means "no signal" or "no reading".
- The value is the one the driver already latched for that frame (SX1276 at the first byte, SX1262/LR1121 at sync word, CC1101 on read). Nothing new is measured, and the level is not re-read after RX_DONE, which would report an empty channel.
- `forward_meters` applies as it does to telegrams: a meter that is filtered out publishes no RSSI either.
- Independent of `diagnostic_mode`. The `last_rssi` / `win_avg_rssi` fields inside the diagnostic payloads are unchanged and remain the tool for reading the board's RF picture.
- Paired with the wMBus MQTT Bridge add-on, each receiving board produces its own signal-strength entity for the same meter, which is what makes two boards comparable.
- All `*_commented.yaml` examples now annotate every optional setting with `# default: <value>`, and `tests/ci/check_example_defaults.py` (wired into CI) holds those annotations and the `Domyślnie` column of `CONFIG_REFERENCE_MINIMAL.md` to the schema. A default changed in `__init__.py` now fails the build instead of quietly outdating ten files.
- `CONFIG_REFERENCE_MINIMAL.md` gained the sixteen options that had a schema default but no entry, plus a section on what per-meter RSSI does and does not tell you.

---

## Fix: recover marginal S1 frames from Manchester erasures

- S1 now retains invalid Manchester-pair positions. When ordinary Format-A CRC validation fails, it tries both bit values independently per CRC block and accepts only a unique CRC-valid assignment.
- The search is capped at eight erasures per block (256 assignments). Larger, unsolved, or ambiguous blocks remain `dll_crc_failed`; T1 and C1 are unchanged.
- Measured on the two real 85-byte captures from 2026-08-14: maps `[3,1,0,3,0,0]` and `[2,1,0,2,2,3]` were restored byte-for-byte to the transmitted frame in 16 and 12 CRC trials.
- Host regressions include multi-block recovery, rejection above the cap, and both real RAW captures.

---

## Diagnostics: how the invalid pairs of an S1 frame spread over its CRC blocks

### Added
- Under `diagnostic_verbose`, every S1 frame candidate the header search reports now also gets an erasure map: `S1 erasure map 1: 6 erasures in 776/776 frame pairs, per CRC block [1,1,1,1,1,1], worst block 1 (2^1 tries)`.

### Why
- An invalid Manchester pair (`00`/`11`) is a known error *position*, not an unknown bit. The decoder substitutes a zero and moves on, throwing that information away. Resolving one such position against the CRC means trying both values, and format A checks each block on its own — so the cost for a frame is 2^(worst block), never 2^(total). Six erasures spread one per block are six two-try problems; the same six inside one block are 64 tries. Which shape real receptions have is unknown, and the total that was already being logged cannot tell them apart. This measures it before anything is decided about the decoder.
- Counted over the frame window only. `symbols_invalid` on the decode path is counted over the whole capture, so at the 512-byte cap it mostly describes the noise trailing the frame: 246 bytes of noise contribute roughly 492 invalid pairs by themselves.

### Notes
- Printed for every reported candidate, not for the top one alone. Candidates are ranked by invalid pairs per checked pair, so a coincidence over a short implied frame can outrank a real header; the erasure map is what separates them.
- Verbose-only, and only on the SX1262 raw-capture path, which is a search over up to 512 chip offsets already. Nothing on a normal receive path changed.
- The block walk was cross-checked against `s1_raw_len_from_l_` for every L-field value, and the bucketing exercised on synthetic Manchester captures with erasures at known byte positions: block boundaries, six clustered in one block, a frame starting away from chip 0, and a capture ending mid-frame.

### Not verified
- Nothing about decoding changed, and nothing here says erasure resolution would recover a frame. That is the open question this log line exists to answer. The expectation that it could be worth a few dB is arithmetic over substitution counts, not a measurement, and it collapses entirely if frames turn out to arrive either clean or with dozens of erasures and nothing in between.
- Not yet run against a live capture.

---

## Fix: a one-tick notify wait could expire before it had waited

### Fixed
- While reading a frame, the receive path waited `pdMS_TO_TICKS(1)` for the next byte before concluding the radio had nothing more to give. A FreeRTOS block time is counted in ticks and expires at the *next* tick interrupt, not one full period later, so a one-tick wait issued shortly before that interrupt returns almost immediately. At `CONFIG_FREERTOS_HZ = 1000`, which is what ESPHome configures, "1 ms" was in practice a random draw from 0–1 ms, and the decision to stop reading could come from tick phase rather than from an empty FIFO. All six waits now use two ticks (`WMBUS_NOTIFY_WAIT_MS`), which guarantees one whole period.

### Notes
- This wait is the second line of the read path, not the first. Each driver's `read()` first polls the FIFO against a hardware-timer deadline (1000 µs on SX1276, 1800 µs on CC1101) that no tick rate or scheduler can shorten, so the floor was already a hardware-timed millisecond and a truncated notify wait cost the tail of it rather than the whole thing.
- Cost: up to about 1 ms more on the paths that give up, up to about 3 ms for the S1 raw read, which allows three idle rounds. Nothing on a successful receive path gets slower.
- Changing only the default argument would have been cosmetic — every caller passed `1` explicitly.
- The same quantization surfaced elsewhere as an actual receive regression on ESPHome 2026.7.1 and later, in a component whose byte loop depends on this wait as its *first* line. That diagnosis (`IoTLabs-pl/esphome-components`, commit `72e76be`) is what prompted this audit, and the credit belongs there.

### Not verified
- Reasoned from FreeRTOS block-time semantics and the tick rate ESPHome sets, not from a before/after count of `payload_read_failed` on hardware. No misbehaviour was observed here that needed fixing; this removes a known way for a read to be abandoned early rather than a symptom that was reported.

---

## Fix: CC1101 modules reporting VERSION 0x04 were refused at startup

### Fixed
- The CC1101 startup self-check compared the `VERSION` status register against a single value, `0x14`. A chip reporting `0x04` failed the check, which called `mark_failed()` — the radio never started, and `dump_debug_status()` labelled it `UNEXPECTED_CHIP_ID`, a verdict that pointed at the wrong thing. `0x04` and `0x14` are two silicon revisions of the same part; both are now recognised.
- Reported by a user on Discussions running a CC1101 whose `VERSION` reads `0x04`.

### Changed
- The revision byte no longer gates the receiver at all. `PARTNUM`/`VERSION` are read and logged; a value outside the known set produces a warning and startup continues. `config_ok` in the diagnostic dump no longer includes chip identity, and the `UNEXPECTED_CHIP_ID` verdict is gone — with it out of the way the classifier reports the state that actually matters (GDO mapping, packet mode, RF profile, RX state) instead of stopping at the first byte it did not recognise.
- A silent SPI bus still fails setup: `VERSION` reading `0x00` or `0xFF` is not a revision, it is a bus with nothing on it.

### Why the check was safe to drop
Chip identity was never what the self-check proved. Nineteen registers are read back and compared against the wM-Bus profile the component just wrote — GDO mapping, FIFO threshold, packet mode, sync word, modem, AGC, front end. Anything that echoes those values from those addresses is a CC1101; anything that does not is rejected on the register check, whatever it claims in `VERSION`.

### Not verified
The fix is reasoned from the register semantics and reviewed, not measured: the author has no `VERSION=0x04` part to test on. On `0x14` hardware the behaviour is unchanged apart from the log wording.

---

## Change: halve the S1 capture budget once the header is known undecodable

### Changed
- An S1 stream capture now stops at 256 raw bytes instead of 512 from the moment `s1_expected_raw_len_()` has looked at the header and produced nothing. The full 512-byte budget is kept whenever a length is derivable.
- The moment is knowable: `s1_expected_raw_len_()` only ever reads raw bytes 0..3, and those do not change during a capture. Once it has seen them and failed, it will fail for the rest of that capture, so the frame is already lost and the only remaining question is how long to stay deaf gathering evidence about it.
- 512 bytes is 127 ms, measured against an air rate of 244 us per raw byte. Half of that is still ample to see what the stream looked like, and hands the receiver back 63 ms sooner - which matters on a band where a second transmission can follow closely.

### Notes
- The cap is deliberately not lowered unconditionally. A legitimate long S-mode frame needs the full budget: an L-field of 150 works out to roughly 340 raw bytes, which a fixed 256-byte cap would truncate. Only captures that have already failed are shortened.
- Measured alongside: at -82/-85 dBm the errors in a marginal capture begin in the C-field or later and the length is still derived; at -90/-95 dBm they are already in the L-field. Nothing can rescue the second case, and nothing should try - a substituted bit in L yields a wrong length rather than a recoverable one.

---

## Fix: one bad chip in the C-field threw away the whole S1 capture

### Fixed
- `s1_expected_raw_len_()` required all sixteen Manchester pairs of the L- and C-fields to decode. The C-field now tolerates up to two invalid pairs and is matched against 0x44 / 0x46 on the bits that did decode; the L-field still demands perfection, because its value cuts the capture and a substituted bit there produces a wrong length rather than a recoverable one.
- The C-field exists only to stop the complemented polarity selecting a plausible but wrong L-field. It contributes nothing to the length, so demanding it decode perfectly bought nothing and cost a great deal.

### Measured
At the sensitivity threshold on 2026-08-01, two captures of the same 85-byte transmission 30 seconds apart:

| RssiSync | first bytes | outcome |
|---|---|---|
| -88 dBm | `66 65 65 65 …` | `exit=s1_length`, 194 B captured, decoded, one bad pair in 776 |
| -89.5 dBm | `66 65 65 6D …` | `exit=buffer_cap`, 512 B captured, lost |

- One chip error, in raw byte 3, turned `0x65` into `0x6D`. That byte is the second half of the C-field. No length could be derived, so the capture ran to the cap and collected roughly 85 ms of post-frame noise on top of a 47 ms frame.
- The measurement consequence was worse than the lost frame. A successful capture reports invalid pairs out of 776, a failed one out of 2048 with three quarters of it noise, so the same signal degrading by a fraction of a dB appears to fall off a cliff. That artefact made the S1 failures look categorically different from marginal reception when they were the same thing.

### Notes
- This does not make a transmitter below the noise floor decodable. It recovers frames at the very edge where the unlucky chip landed in the header instead of the payload, and it makes `symbols_invalid` and the capture-quality figures measure signal quality rather than whether the header happened to survive.

---

## Result: S1 bandwidth sweep finished, 234.3 kHz is the optimum

### Changed
- The S1 receive bandwidth returns to 234.3 kHz and stays there. The two test commits that moved it to 312 kHz and then 156.2 kHz have served their purpose.

### Measured
Longest run of valid Manchester pairs in an SX1262 capture, each taken on a transmission an SX1276 decoded in the same second, out of the 680 pairs an 85-byte telegram needs. Random data gives about 11.

| RX bandwidth | longest valid run |
|---|---|
| 312.0 kHz | 30 pairs |
| **234.3 kHz** | **191 pairs** |
| 156.2 kHz | 47 pairs |

- A genuine peak, not a trend: both directions cost a factor of four to six. Widening admits noise, narrowing starts cutting a signal whose occupied spectrum is wider than either Carson's rule (133 kHz) or Semtech's sizing rule (143 kHz) predicts for a Manchester-coded BT=0.5 chip stream.
- The 2026-07-30 finding that 156.2 kHz "stopped reception entirely" is now qualified: it does capture, it captures worse. That earlier measurement was taken before the AN1200.53 capture fix, when no bandwidth decoded anything.

### Notes
- At the optimum the capture still holds 191 of the 680 pairs a frame needs. Bandwidth is worth a factor of four here and no more; it is not what stops the SX1262 decoding S-mode. The sweep is closed and does not need re-running.

---

## Test: S1 bandwidth sweep, third point at 156.2 kHz

### Changed
- The S1 receive bandwidth moves from 312 kHz to 156.2 kHz. Third measurement point of a sweep, still a test. C1 keeps 234.3 kHz and is not part of this.

### Measured
Longest run of valid Manchester pairs in an SX1262 capture, taken on the same transmission an SX1276 decoded in the same second, out of the 680 pairs an 85-byte telegram needs (random data gives about 11):

| RX bandwidth | longest valid run |
|---|---|
| 312.0 kHz | 30 pairs |
| 234.3 kHz | 191 pairs |
| 156.2 kHz | this build |

- Widening to 312 kHz made recovery six times worse, which disposes of the argument that a filter narrower than the signal was smearing chip edges. What remains is ordinary noise bandwidth, and the trend between the two measured points runs towards narrower.
- 156.2 kHz was tried once before, on 2026-07-30, and reported as stopping S1 reception entirely. That was measured before the AN1200.53 capture fix, when no bandwidth decoded anything - the same objection that justified re-testing 312 kHz.

---

## Test: S1 on SX1262 goes back to the 312 kHz receive bandwidth

### Changed
- The S1 receive bandwidth returns from 234.3 kHz to 312 kHz. This is a test, marked as one, to be reverted if it changes nothing. C1 keeps 234.3 kHz - it has no comparable history and decodes normally.
- Why re-run a setting that was already replaced: the AN1200.53 capture fix landed 38 minutes after the move to 234.3 kHz. Every wide-bandwidth test therefore ran on a broken capture path, and every fixed-capture test ran narrow. 312 kHz has never been tried with the receive path in its current state.
- Why the original argument for narrowing looks wrong: it was about noise, and 234.3 kHz does admit about 1.25 dB less than 312 kHz. But that is a sensitivity argument and says nothing about distortion. The measurements run the other way - 156.2 kHz stopped S1 reception outright, and 234.3 kHz syncs on real S-mode frames and has never once decoded one. Carson's rule gives 133 kHz for this signal and Semtech's sizing rule 143 kHz; a setting above both killed reception completely, so both under-describe the occupied spectrum of a Manchester-coded BT=0.5 chip stream. A filter narrower than the signal does not only reject noise, it smears the chip edges.

### Notes
- An earlier note in this file claimed S1 kept the wide 312 kHz bandwidth. That stopped being true on 2026-07-30 and the note was left stale for two days; it is corrected here.

---

## Diagnostics: find where the S1 frame actually starts in a capture

### Added
- Under `diagnostic_verbose`, every S1 stream capture on the SX1262 is searched for the chip offset at which a valid L+C header actually sits - all offsets up to 512 chips, both Manchester polarities - and the result is logged with the frame length and the number of invalid pairs inside the frame itself.
- `s1_expected_raw_len_()` assumes the frame begins at chip 0, because the radio strips the sync word in hardware and the payload should follow immediately. Every S1 capture on this driver ends at `exit=buffer_cap`, which is exactly what happens when no length can be derived from those first bytes.
- What prompted it: three captures of the same repeater transmission, decoded identically by an SX1276 in the same second on 2026-08-01, produced three completely different first bytes here - `99363510…`, `998A9A9A…`, `DDFBDA9A…`. Identical air content, different buffer content.

### Notes
- The output separates three cases that were previously indistinguishable. A stable `chip=0` clears the capture path and points at demodulation. An offset that moves between captures locates a start-alignment bug. No valid header anywhere means the frame is not in the capture at all.
- The invalid-pair count is deliberately taken over the frame body only. Counted across the whole 416-byte buffer it is dominated by the post-frame noise the capture keeps collecting, which is what made earlier readings of that number misleading.
- Nothing is changed in how captures are taken. This only looks at what was captured.

---

## Fix: the frequency-error readout described noise, not the decoded frame

### Fixed
- `RegAfc` and `RegFei` are now sampled once per frame by `latch_frame_metrics_()`, at the moment its first bytes reach the FIFO, together with RSSI. `dump_debug_status()` reports the latched values and how many seconds old they are, and no longer re-reads the registers.
- The previous version read them live inside the dump. That dump runs on a receive-wait timeout - by definition when nothing has arrived for a minute - so it returned whatever noise last tripped the preamble detector. Measured on 2026-08-01 on a node decoding one transmitter every 123 seconds: +23.5 kHz two seconds before a frame, then -17.6 kHz corrected with a +68.5 kHz residual on a preamble that never matched sync, then -16.1 kHz three seconds before the next frame. None of those came from the transmitter being decoded.
- RSSI is copied into a separate sticky field for the diagnostic. `restart_rx()` resets the value handed to the packet to the not-measured sentinel on every re-arm, which is correct there but would have made the dump report -127 for a frame whose level was measured.

### Notes
- The general shape of this is the same mistake twice: a register that is only valid at one instant, read at another instant, producing a number that looks authoritative and describes nothing. The RSSI sampling fix in July had exactly this cause.

---

## Diagnostics: SX1276 reports the frequency error of the last reception

### Added
- `dump_debug_status()` now prints `RegAfc` and `RegFei` with both the raw register value and the converted offset in Hz. `RegFei` is what the receiver measured, `RegAfc` is what the AFC actually corrected by; both are latched from the last received frame, so on a link mode where frames are minutes apart the reading still describes the last real transmitter rather than noise.
- The reason it is worth having: the SX126x has no AFC in GFSK at all, so whatever offset this register reports is error an SX1262 has to swallow whole. Measured on T1 on a LilyGO T3-S3 on 2026-08-01, the AFC was correcting −37.8 kHz on live meters.

### Notes
- This turns a frequency sweep into a single read. Instead of retuning a receiver in steps and watching whether reception improves, the offset of the transmitter that was actually decoded can be read off directly and applied once.

---

## Fix: SX1276 ran with the high-frequency LNA boost off

### Fixed
- `RegLna` (0x0C) is now written as `0x23`: maximum gain plus `LnaBoostHf`. The driver never wrote the register at all, so it ran on the reset default `0x20` - same gain, boost off. Confirmed on hardware 2026-08-01, a register-bank dump of a LilyGO T3-S3 read `0x0C = 0x20`.
- `0x23` is Semtech's own value. It is what LoRaMac-node puts in `RADIO_INIT_REGISTERS_VALUE` for every FSK board, and the omission was found by diffing this driver's setup sequence against that table rather than by observing a symptom.
- The gain half of the register matters less than it looks: `AgcAutoOn` is set immediately afterwards, so the AGC drives `LnaGain` itself. `LnaBoostHf` is the part that persists - it raises LNA current by 50% for a better noise figure. It applies above 525 MHz and is ignored below, so the write is unconditional.

### Notes
- This is a sensitivity change on every listen mode, not just S1. It has not been measured here; the argument for it is that the manufacturer's reference driver sets it and this one did not. Frame counts before and after on an unchanged node are the way to find out whether it is worth anything.

---

## Note: on the SX127x in FSK, RegOpMode does not tell you whether RX is running

After writing RX (`0b101`) to `RegOpMode`, the SX1276 reads the register back as `0b100` - FSRX, frequency synthesis with the receiver off - and reports `ModeReady` and `RxReady` clear in `RegIrqFlags1`, on a receiver that is working normally. Measured on a LilyGO T3-S3 on 2026-08-01: a node in that exact state decoded three T1 frames at -75, -91 and -95 dBm within the same second, and printed the register readback in between.

This is known behaviour, not a board fault. RadioLib's `SX127x::setMode()` masks the low mode bit out of its write verification for FSK RX specifically, with the comment "disable checking of RX bit in FSK RX mode, as it sometimes seem to fail (#276)".

### Notes
- Nothing in the driver tests the mode readback any more. `dump_debug_status()` still prints `RegOpMode`, because the value is worth seeing, but it no longer derives a `receiver_running` claim from it and no longer warns that nothing can be received. An earlier version of that warning fired for hours on a receiver that was decoding frames while it fired.
- A `ModeReady` wait was added to the S1 arming path on the strength of that reading and has been removed again. It was polling for a bit that does not come back even when the transition succeeds, and it cost 2 ms of busy-waiting twice per re-arm.
- The general lesson is the one this project already applies to RSSI: a register that reports something impossible is worse than a register nobody reads, because the impossible value still gets reasoned about.

---

## Fix: `clear_device_errors_on_boot` did nothing on a node that received nothing

### Fixed
- The SX1262 device-error clear has moved from `capture_rx_stream_()` to `setup()`. It was gated on the first captured frame, so on a node receiving normally it ran within seconds and nobody ever saw the flag, while on a node receiving nothing it never ran at all - leaving `clear_device_errors_on_boot: true` inert in the one case where the error register is the only thing left to read. Observed on hardware 2026-08-01: two SX1262 nodes reporting `XOSC_START_ERR` for minutes on end while sitting in RX with a correct configuration.
- `XOSC_START_ERR` is set during the power-on sequence as a matter of course on a TCXO board, because the chip tries to start its crystal oscillator before DIO3 has been told to power the TCXO - DIO3 is configured after reset. Clearing it once the reference is set up is what the datasheet expects. Paired with the re-read at the end of `setup()`, the flag now means something: one that clears was a power-on artefact, one that comes back after a clean clear is a reference that genuinely is not starting.
- Removing the block from the receive path also takes a 7 ms blocking delay off the first capture.

### Notes
- This entry originally also announced an SX1276 warning derived from reading `RegOpMode` back after arming RX. That reading turned out not to mean what it looked like and the warning has been removed - see the note about `RegOpMode` in FSK RX above.

---

## Diagnostics: repeatable radio state instead of one snapshot per boot

### Added
- `dump_debug_status()` is now implemented for SX1276 and SX1262. It was declared on the base transceiver and implemented only by the CC1101, so the two SX drivers inherited an empty method: their registers were readable exactly once, at boot, and never again. The component already calls this on every receive-wait timeout when diagnostics are verbose, so a silent node now reports its own state roughly once a minute.
- SX1276 reports `RegOpMode` decoded to a mode name, `RegIrqFlags1/2`, RSSI, `RegRxConfig`, `RegRxBw`, `RegPreambleDetect`, the sync configuration and the live sync word, plus the DIO1 level. `preamble_detected` and `sync_matched` are restated in words, because the two bits that say whether anything is arriving are one bit each in the middle of a hex byte. A chip sitting in `FSRX` instead of `RX` - synthesiser locked, receiver off - now says so with a warning instead of looking configured.
- SX1262 reports the `GetStatus` chip mode, latched IRQ status, device errors, `RegRxGain`, the live sync word, the stream pointers and the DIO1/BUSY levels, with the same restatement and the same warning when the chip is not in RX.
- The verbose flag is pushed into the driver before the receiver task is created, so no frame is handled while the component and the driver disagree about how much to report.

### Changed
- The SX1262 RSSI provenance snapshot (`IRQ=... captured=... first[8]=...`) repeats on every capture when diagnostics are verbose, instead of once per receive path per boot. One sample is the right volume when frames are decoding; it is useless when nothing is, because a single `first[8]` cannot separate a real frame captured out of alignment from the detector firing on noise. That needs a series. Outside verbose mode the one-shot behaviour is unchanged.
- A snapshot is dropped rather than overwritten if the previous one has not been drained yet. Writing into the slot while the main task copies out of it would produce a log line built from two different captures, which is worse than a missing line precisely because it still looks like data.

### Notes
- Both changes come from a day spent comparing three receivers with no repeatable register reads. Boot-time-only diagnostics answer "did the chip answer over SPI"; every question that matters when reception stops is about the current state.

---

## Fix: SX1262 was never recalibrated after the TCXO was enabled

### Fixed
- `SetDIO3AsTcxoCtrl` hands the reference over to the TCXO, but DIO3 is what powers that TCXO - so every calibration the chip performed at power-on ran against the internal RC oscillator, a reference that stops existing one command later. The SX1261/2 datasheet requires the calibration to be relaunched afterwards. `Calibrate(0x7F)` is now issued right after the TCXO is enabled, recalibrating RC64k, RC13M, PLL, the three ADC blocks and the image. This runs only when `has_tcxo: true`, so boards without a TCXO are untouched.
- Missing this step does not fail loudly. The radio starts, arms RX and receives a transmitter on the same desk perfectly well; what it loses is the last few dB, which is the band real meters arrive in. Nothing in the log distinguished that from a bad antenna.
- `GetDeviceErrors` is now read once at the end of `setup()` and logged, with `XOSC_START_ERR` and `PLL_CALIB_ERR` raised to `ESP_LOGE`. A device-error snapshot already existed, but it lives in `capture_rx_stream_()` and fires on the first captured frame - never, in the one case worth diagnosing, where nothing is being received.

### Notes
- Found while comparing an SX1276 against an SX1262 on S-mode. It is not the explanation for that comparison and should not be credited with fixing it; it is a datasheet requirement that was missing on its own terms.

---

## Fix: a frame could be logged as "RSSI: 0dBm"

### Fixed
- `sx126x_rssi_dbm_()` treated raw 0 as "register never written" and converted everything else. Raw 1 is -0.5 dBm, and integer division turns that into a clean `0 dBm` - a level no wM-Bus frame can arrive at, since the front end saturates around -5 dBm and a transmitter on the same desk at minimum power still lands tens of dB below. Observed on hardware: a correctly decoded frame reported as `RSSI: 0dBm`. The conversion now rejects the whole impossible top of the scale (raw below 20, i.e. above -10 dBm) and returns the not-measured sentinel instead.
- The plausibility rule lives in the conversion, not at each call site. The three places that read a level - in-flight sampling, packet status on the FIFO path, packet status on the stream path - ask for a converted value and take the first one that is not the sentinel. They previously tested the raw byte for `!= 0`, which let an implausible sync reading shadow a usable average.
- `Packet::rssi_` defaulted to 0. That is not a sentinel but a reading, and the same impossible one: a packet whose level was never set reported a perfect signal instead of admitting it had none. It now defaults to -127, which the statistics already know to exclude.

### Notes
- A fabricated number is worse than a missing one. This was found while comparing two receivers, where `RSSI: 0dBm` sat in the middle of a measurement session and had to be argued about before it could be dismissed.

---

## Note: S1 on SX1262 keeps the wide 312 kHz receive bandwidth

A narrower window was tried and reverted. Carson's rule puts the S-mode requirement at 132.8 kHz - 2 * (fdev + chiprate/2) for a 32.768 kchip/s Manchester stream - so S1 was moved from the inherited 312 kHz down to 156.2 kHz, expecting about 3 dB less noise. On hardware the opposite happened: a Heltec V4 stopped receiving S1 frames altogether while an SX1276 beside it kept decoding the same transmitter.

Carson under-describes a Manchester-coded signal. The chip stream carries real energy past the nominal deviation, and the datasheet bandwidth is a -3 dB figure rather than a flat passband, so the usable window is narrower than the number suggests. The code keeps 312 kHz for S1 with a comment saying not to narrow it again without measuring the received spectrum first.

---

## Fix: the summary payload buffer belongs in .bss, not on the loop stack

### Fixed
- Growing the summary payload buffer to 3072 B put 3 kB on the loop task stack inside a call chain that then nests `maybe_publish_suggestion_()` (another 640 B buffer) and the logger, which writes through newlib and the VFS. On a LilyGO SX1276 node this crashed with `Fault - LoadProhibited` in `esp_vfs_write`, reached from an `ESP_LOGI` that only formats string literals - the signature of a stack overflow, not of a bad pointer. It fired immediately after the first 60 s summary, on the node whose suggestion path actually runs.
- The three summaries now share one static buffer. They are called only from `Radio::loop()`, one after another and never concurrently, so they cannot clobber each other. This costs 3 kB of `.bss` and takes 3 kB off the loop stack - less stack than the code used before the buffer was ever enlarged.

---

## Feature: S1 gets the same diagnostics as T1 and C1

### Added
- Summaries publish an `s1` block next to `t1` and `c1` - `total`, `ok`, `dropped`, `per_pct`, `crc_failed`, `crc_pct`, `avg_ok_rssi`, `avg_drop_rssi` - plus `manchester_drop` and `manchester_pct`. The per-mode counters are indexed by link mode and S1 frames were always written to them; nothing read them back, so S1 traffic was invisible in every summary except the global totals.
- Hints `S1_WEAK_SIGNAL`, `S1_INTERFERENCE_OR_RX` and `S1_OVERLOAD_OR_MULTIPATH`, mirroring the C1 branches, and `S1_MANCHESTER_ERRORS` when at least 20% of S1 frames die at the Manchester stage. S-mode is Manchester coded, so that stage is the analogue of T1's invalid 3-of-6 symbols.
- `dropped_by_stage` gains `s1_precheck`, `s1_manchester`, `s1_l_field` and `s1_length_check`. The parser has emitted these stage names all along, but `bucket_for_stage_()` did not recognise them, so every S1 failure was filed under `other`.
- `stage_rank_()` in the parser now lists the `s1_*` stages alongside their T1/C1 equivalents. This changes nothing today - `try_parse_s1_()` runs only on the forced-S1 path, which returns before the ranking is consulted, and an S-mode frame cannot reach a radio tuned for T1/C1 anyway. It is there so the ordering is right if an S1 result ever gets compared with another parse attempt.

### Fixed
- The summary payload buffer was 2048 B while the JSON needs roughly 2.2 kB once a long hint text is included, and more as counters grow. `snprintf` truncated it silently and published invalid JSON. The buffer is now 3072 B and a truncation logs a warning instead of shipping a broken payload. This could already happen before the `s1` block was added, with any of the longer C1/T1 hints.

---

## Fix: a window where every frame failed CRC was reported as "looks good"

### Fixed
- `DIAG hint` starts as `OK / "looks good"` and each branch only overrides it. No branch covered a window where frames arrived but none decoded: the mode-specific branches key on the `c1_*` and `t1_*` counters, which stay at zero under `listen_mode: s1`, and the generic weak-signal branches need `avg_drop_rssi <= -90`. A real S1 window - `total=1 ok=0 dropped=1 crc_failed=1` at -87 dBm - therefore reported `OK | looks good`, and at INFO level, while an empty window is logged as a warning. A window that failed completely read better than a quiet one.
- New hint `ALL_DROPPED` covers `ok == 0`. It sits after the specific branches, so `C1_WEAK_SIGNAL`, `T1_BITFLIPS` and the rest keep priority and only the wrong default is replaced. Like every non-`OK` hint it is logged as a warning. It applies to all three summary windows (60 s, 15 min, 60 min).
- The `ADD_HIGHLIGHT_METERS` suggestion fired on frames *arriving* (`total > 0`), so the same window advised "Meters are being received. Check which IDs appear in wmbusmeters" although nothing decoded and no id was ever published - sending the user to look for something that does not exist. It now requires at least one decoded frame in the window.

### Notes
- The hint tree still has no counters of its own for S1; only `c1_*` and `t1_*` exist, so S1 windows fall back to generic diagnoses. That is a separate change.

---

## Fix: XIAO with Wio-SX1262 received through a disabled antenna switch

### Fixed
- The Seeed Wio-SX1262 does not connect its antenna unconditionally. Module pin 1 (`RF_SW`, "External IO control internal gate RF switch") must be held high by the host; on the XIAO ESP32S3 kit it is GPIO38. Nothing drove it, so the pin idled as a high-impedance input and the receiver ran on leakage alone.
- This is separate from `dio2_rf_switch`, and both are needed. Per the module datasheet the SX1262's own DIO2 chooses the TX/RX *direction* (high = TX, low = RX); `RF_SW` decides whether the switch conducts at all.
- The XIAO examples did carry a workaround - an `on_boot` action toggling a `gpio` output on GPIO38 - and it never worked. Priority 900 lands in the same ESPHome setup bucket as the `gpio output` component itself, so ordering falls out of registration order and the write happened before the pin was an output. No warning, no error, no log line. It has been removed from both examples.
- Measured on hardware, same board and antenna before and after: meter `00088888` went from -96 dBm to -68 dBm, and the receiver went from 4-6 frames per minute across 3 meters to 14 across 32.

### Added
- `rf_sw_pin` for `SX1262`. The pin is driven high inside the radio's own setup, before the chip reset, where ordering is guaranteed. Boards whose module gates the antenna path this way need it; Heltec V3/V4/V4-R8 do not - they use the `fem_*` pins and are unaffected.
- The XIAO example joined the CI build matrix. That board had never been compiled in CI, and it is the only config exercising the new option.

### Notes
- **Action required on XIAO ESP32S3 + Wio-SX1262.** Add `rf_sw_pin: GPIO38` and delete any earlier `output:` / `on_boot:` block driving GPIO38 - leaving both makes ESPHome reject the config on a duplicate pin declaration. Without the option the receiver keeps running roughly 30 dB deaf.
- The symptom is not silence. Frames still decode, `DIAG hint` still reports `GOOD`; there are simply several times fewer of them and every RSSI sits in a narrow band just above the sensitivity floor.

---

## Fix: SX1262 reported the noise floor as every frame's RSSI

### Fixed
- Every received frame reported the same near-floor RSSI regardless of meter or distance, because the level was read after the transmission had already ended. Both receive paths fell back to an instantaneous `GetRssiInst` that ran after `RX_DONE` and after the whole buffer had been drained, so it measured the empty channel. The constant value was not a placeholder - it was a real measurement of the noise floor.
- In the streamed path (AN1200.53) that fallback was taken every time: the procedure deliberately never lets the packet engine finish a packet, so `GetPacketStatus` never latches values for the frame being captured.
- The driver also preferred the wrong register. `RssiAvg` averages over the whole receive window, which is a fixed 255 bytes, so for a 134-byte wM-Bus frame more than half of it is post-transmission noise. Measured on one frame: `RssiSync` -97 dBm against `RssiAvg` -117 dBm.
- RSSI is now sampled while the frame is still on air - a peak-hold taken as bytes arrive in the streamed path, `RssiSync` in the FIFO path. The post-frame instantaneous read is gone.
- Scaling was written as `-((int) raw) >> 1` instead of `-raw / 2`; unary minus binds tighter than the shift, so it arithmetic-shifted a negative value and floored instead of truncating.
- A frame whose level cannot be sampled now reports -127 dBm ("not measured") instead of a fabricated value, and such samples are excluded from the per-meter and diagnostic averages rather than dragging them down.

### Notes
- **Historical RSSI data is not comparable with new data** on any SX1262 board. Per-meter averages, `win_avg_rssi` and every RSSI-derived diagnostic need to be re-collected after the update.
- The first frame on each receive path logs the raw radio values at INFO, so the active source can be confirmed on hardware without raising the log level.

---

## Fix: meters with a non-BCD ID can be matched by forward_meters and highlight_meters

### Fixed
- Meter matching decoded the A-field as BCD and gave up otherwise, so meters that do not use a BCD ID (Diehl/IZAR among others) had no usable ID at all. They could never be listed in `highlight_meters`, and with `forward_meters` active their telegrams were dropped silently - the one case where the whitelist discarded frames the user could not get back by any configuration.
- Both options now also match the raw A-field value, written the way the log prints it: `id:417F0666` is configured as `"0x417F0666"` (quoted, or YAML turns it into a number). Decimal entries keep their existing meaning, so no configuration changes behaviour.
- The two forms are told apart without ambiguity: a non-BCD A-field always contains a nibble above 9 and therefore always prints a hex letter, while a BCD ID never does. The `0x` form works for BCD meters too (`"0x00088888"` is meter `88888`).
- Per-meter statistics were keyed on the BCD ID, so every non-BCD meter collapsed into a single shared entry at key 0. They are now keyed on the raw A-field value, which is unique for every meter.
- `target_meter_id` still accepts only BCD IDs. A hex value there used to be accepted and then never match; it now logs a warning at boot pointing to `forward_meters`.

---

## Fix: NO_METERS_DETECTED no longer fires on a quiet summary window

### Fixed
- `NO_METERS_DETECTED` claimed a wiring or radio-configuration fault whenever a single summary window contained no frames. That counter (`diag_total_`) is reset after every window, so a receiver that had been working all day still reported `total == 0` for any quiet minute - and meters are routinely quiet at night. The suggestion is now limited to receivers that have not seen a single frame since boot, and additionally suppressed for the first 5 minutes of uptime, where silence carries no information because meters transmit tens of seconds to several minutes apart.
- A receiver that used to work and then went silent is a different diagnosis and stays with the health pulse (`sec_since_last_rx`); it was never what this suggestion measured.
- The `NO_DATA` summary hint said "no packets received yet", which reads as "nothing ever arrived" even though it describes a single window. It now says "no packets in this window". The machine-readable `hint_code` is unchanged.

---

## Feature: forward_meters - forwarding whitelist for the RAW telegram topic

### Added
- `forward_meters` limits which meters are published to `wmbus/<topic_name>/telegram`. Useful where most of the received traffic belongs to neighbouring meters and only a few are your own.
- Accepts an explicit list of meter IDs, or `true` to reuse the IDs already listed in `highlight_meters` so the same list is not written twice.
- An empty list (the default) or `false` forwards every decoded frame, so existing configurations are unaffected.
- The parsed IDs are logged from `setup()` and again in the delayed boot status block, next to the YAML sanity output. The repetition is deliberate: `setup()` runs before the network logger attaches, so over `esphome logs` only the second one is visible. `dump_config()` reports the same state, but is filtered out at `logger: level: info` - it needs `level: config` or higher.

### Notes
- The filter runs after decoding and the DLL CRC check, so it matches a meter ID the parser has already validated. Filtering on the raw header would be cheaper but unreliable: an ID read from a frame that failed CRC can be corrupted.
- Matching uses the BCD-decoded 8-digit meter ID. A meter whose log line shows a hex ID (`id:417F0666`) has a non-BCD A-field and cannot be whitelisted - it is dropped while the filter is active.
- `forward_meters: true` with an empty `highlight_meters` does not silence the stream: filtering stays off and a configuration warning is printed at boot.
- Diagnostics are unaffected. Counters and RSSI statistics are updated before publishing, so summaries still cover the whole ether including neighbours; only the RAW stream is reduced.
- `target_meter_id` keeps its own topic and is deliberately not subject to the whitelist.

---

## Fix: guard gmtime()/strftime() in rtlwmbus timestamp

### Fixed
- `Frame::as_rtlwmbus()` now guards `std::gmtime()` returning `nullptr` and `std::strftime()` returning `0`, falling back to a fixed `1970-01-01 00:00:00.00Z` timestamp. Prevents a potential null-dereference / unterminated-buffer read when the system clock holds a `time_t` value that cannot be represented (e.g. an unset or out-of-range clock). Hardening only — no wire or format change during normal operation.

---

## Current documentation note

### Added
- Experimental S1 receive mode.
- `listen_mode: s1` uses a dedicated S1 receive path and does not fall back to T1/C1 parsing.
- `listen_mode: both` remains T1/C1 only.
- `listen_mode: s1` defaults to `868.300 MHz`; T1/C1/both remain at `868.950 MHz`.
- Explicit `frequency:` in YAML still overrides the mode default.

### Notes
- S1 support is intended for diagnostics and compatibility testing.
- If a valid S1 telegram is received, it is published to MQTT like other validated wM-Bus telegrams.
- Meter-value decoding remains external and depends on the backend driver and encryption key.
- Proprietary or polling-based systems may not produce standard passive S1 telegrams.

---

## Adaptive SX1276 and MQTT diagnostics

**Summary**
Improve adaptive SX1276 behavior, add MQTT diagnostic suggestions, and expand runtime diagnostics.

**Description**
This release improves the real-world behavior of the RAW-only wM-Bus bridge, especially on SX1276 in noisy RF environments.

Main changes:

* improved `sx1276_busy_ether_mode: adaptive` logic so activation reacts to actual reception loss, not just RF noise
* added MQTT `suggestion` events with actionable diagnostic hints and YAML snippets
* added `busy_ether_changed` MQTT events for adaptive state transitions
* expanded diagnostic summaries with new runtime fields, including `busy_ether_state`
* added/expanded `summary_15min`, `summary_60min`, and per-meter snapshot reporting
* fixed multiple logic and documentation inconsistencies discovered during real hardware testing
* fixed: `busy_ether_state` in `/summary` JSON now emits `"n/a"` on SX1262 instead of the misleading `"adaptive_passive"` (the algorithm never ran on SX1262, only the stored mode value was serialised)
* fixed: `hint_code` no longer stays `"OK"` for windows with 11-99% drop rate and no specific diagnosis — new code `MODERATE_DROPS` is emitted instead so elevated drops are always visible as WARN in serial log
* changed: `highlight_meters` per-packet serial log now shows `packet #N received` instead of the previous `stats / statystyki: count=N interval=... avg_rssi=...`; per-meter stats remain available via MQTT `meter_window` events

This version does not change the project architecture: the ESP device still focuses on RF reception and RAW MQTT publishing, while meter decoding remains external.
