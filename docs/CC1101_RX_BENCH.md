# CC1101: separate sync and RX restart experiments

[Polska wersja](CC1101_RX_BENCH_PL.md)

These options test missed strong frames, not a proven sensitivity improvement.
Build all phases from the same dev commit. Both new options default to false.

| `wmbus_radio` option | Default | Effect |
|---|---|---|
| `cc1101_sync_15_16` | `false` | T1/C1: 0x06 (16/16 + CS) versus 0x05 (15/16 + CS — **both keep the carrier-sense gate**, SWRS061I p. 44). S1 is always TI's 0x05 (15/16 + CS), whatever this and `cc1101_carrier_sense` say. Tested 2026-11-16: without CS, noise matched the S1 sync ~230/min (15/16) or ~25/min (16/16), all bad CRC, and no real weak S1 frame came through; a strong fake S1 frame was decoded 714/720 with 0x05. The CC1101 does not hear weak S1 meters that SX1276/LR1121 decode. |
| `cc1101_carrier_sense` | `true` | `false` = sync without carrier sense (0x02, with `sync_15_16` 0x01). YAML only: no longer on the add-on tile since 2026-11-16 (off was measured harmful); a value stored by an older build is dropped at boot. |
| `cc1101_rx_bandwidth` | `232khz` | T1/C1 RX filter: `232khz` (default since 2026-11-16, measured better), `325khz` (TI AN067 link B), `270khz`. YAML only since 2026-11-16; a value stored from the add-on during the test is dropped at boot. |
| `cc1101_rx_state_wait` | `false` | SIDLE → wait IDLE → SFRX → SRX → wait RX. |
| `cc1101_idle_rx_refresh` | `false` | Check approximately every 50 ms while waiting for IRQ; re-arm only with GDO2 low, FIFO empty and state RX. |

Merge into the existing radio block, retaining the board pins:

```yaml
cc1101_sync_15_16: false
cc1101_rx_state_wait: false
cc1101_idle_rx_refresh: false
```

Run separate A-B-A experiments: sync false/true/false with both RX options
false; state_wait false/true/false with sync fixed and refresh false; then
refresh false/true/false with sync and state_wait fixed (prefer state_wait=true
throughout the last experiment). The operator decides when to run each phase.
The sync option changes only the sync tolerance; `cc1101_carrier_sense: false` removes the CS gate.

Use identical hours/day types, at least 2 hours, unchanged common antenna and
LR1121 reference. Deduplicate `(source, boot_id, seq)`; match CRC32 and 20-second
buckets ±1. Analyze T1 separately from C1/S1. Report emissions/meters relative to
LR, LR RSSI bucket coverage, strong-frame losses (-90 < RSSI <= 0) overall and
per meter, LR emissions/hour and restarts.

Configuration reports show both switches. `CC1101 RX bench` status snapshots
show refresh attempts, skipped checks and state timeouts since boot, not lost
frames. State waits require two consecutive matching MARCSTATE reads 20 us
apart, with a 10 ms deadline. Failure to reach IDLE prevents SFRX/SRX; failure
to reach RX is reported. Timeout messages are rate-limited.

Maintenance never runs during frame capture. Repeated GDO2/FIFO/state checks
reduce, but cannot eliminate, the race between the last check and SIDLE.
SPI, calibration and scheduling add to the nominal 50 ms. The roughly 5-second
safety re-arm remains. FOC/BSCFG/PQT/AGC and other radio configurations are unchanged.
These counters cannot prove carrier-without-sync; that requires separate timed
CS/GDO2/FIFO observations. PQT_REACHED with PQT=0 is not preamble evidence.
Host tests do not validate RF; no running board or OTA setting is changed.

Reference: [TI CC1101 SWRS061I](https://www.ti.com/lit/ds/symlink/cc1101.pdf),
section 10.3, SFRX and MARCSTATE.
