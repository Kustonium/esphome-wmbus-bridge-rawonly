# RAM MQTT buffer, per-topic QoS, and the RX timestamp field

[Polska wersja](BUFFER_QOS_TIMESTAMP_PL.md)

This note documents three additions made on top of the upstream RAW-only
bridge, done as preparation for migrating a receiver from
`SzczepanLeon/esphome-components@version_4` to this project:

1. a RAM store-and-forward buffer for the telegram stream while MQTT is
   disconnected,
2. per-topic MQTT QoS (0/1/2), configurable in YAML,
3. confirmation that the reception timestamp already travels next to RSSI —
   no new field was needed.

## 1. RAM buffer (`mqtt_buffer_size`)

Before this change, the project's own docs were explicit: when the local
broker is unreachable, RF reception continues but every MQTT publish is
silently skipped. The telegram is lost, not delayed.

`mqtt_buffer_size` (**default `0` — the feature is off until you set it**,
range `0`-`8192`, or `auto` — see below) sets a RAM FIFO queue.

**The unit is queued MQTT messages, not telegrams.** One received telegram
enqueues two of them: the raw frame on `.../telegram` and its metadata
companion on `.../rx`, which is published for every forwarded frame. So
`mqtt_buffer_size: 64` carries roughly **32 telegrams** through an outage, and
the `buffer_depth` / `buffer_dropped_*` sensors and the per-meter
`buffer_priority` quotas all count in the same messages. Eviction also works
per message, so a full buffer can drop a telegram while its companion stays
queued — the backend then sees metadata it cannot pair with a frame.

The default is `0` because store-and-forward is not a free improvement that
can be turned on for everyone: it changes delivery semantics (a reconnect
replays a burst of telegrams whose `received_at` is minutes old) and it spends
internal heap on boards without PSRAM. An upgraded config must behave exactly
as it did before, so enabling it is a decision made in YAML.
While MQTT is disconnected, the raw telegram publish and its `/rx` metadata
companion (see part 3) are queued instead of dropped; once MQTT reconnects
they are flushed in order, oldest first, a few messages per `loop()` tick so
a large backlog cannot starve radio reception. If the buffer fills up before
reconnecting, the *oldest* queued message is dropped to make room for the
newest one — a long outage keeps the most recent readings rather than
locking onto whichever frame arrived first.

Deliberately **not** buffered: the retained per-meter RSSI scalar
(`wmbus/<topic>/rssi/<meter_id>`), health/meters pulses, diagnostic
summaries/suggestions, the target-meter debug topic, and the raw dev tap.
Those are either "latest value" or "this window" signals where a stale
queued entry would be actively wrong, or best-effort debug aids that were
never part of the data-loss problem this buffer solves.

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: 64   # 0 disables buffering (old behaviour: drop on disconnect)
```

### Runtime control without reflashing

`mqtt_buffer_size` sets the *compiled ceiling*. To inspect and adjust the
buffer live, declare the optional `sensor:`/`number:` sub-platforms and
ESPHome's own `web_server:` with authentication — no custom HTTP server or
auth code was added to this component for this:

```yaml
web_server:
  version: 3
  auth:
    username: !secret web_username
    password: !secret web_password

sensor:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here     # the id: of your wmbus_radio: block
    buffer_depth:
      name: "WMBus Buffer Depth"
    buffer_dropped_total:
      name: "WMBus Buffer Dropped"
    buffer_oldest_pending_age:
      name: "WMBus Buffer Oldest Pending Age"

number:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here
    buffer_capacity:
      name: "WMBus Buffer Capacity"
```

`buffer_depth` is the queued message count, `buffer_dropped_total` counts
drops since boot, and `buffer_oldest_pending_age` is the oldest pending
message age in seconds.

The `number` entity can only lower the effective capacity, never raise it
past the configured ceiling (the current computed ceiling in `auto` mode) — the C++ side clamps and republishes the effective
value, so the portal never claims a capacity larger than the permitted ceiling.

### Is it one shared buffer, or one per meter?

Both, selected automatically by whether `forward_meters` is a whitelist:

- **No whitelist configured** (`forward_meters` empty, the default): one
  shared FIFO queue for the whole receiver, exactly as described above - the
  oldest *message*, from whichever meter, is what gets dropped when the
  buffer is full.
- **Whitelist configured**: each meter has a weighted quota, enforced only
  when the whole queue is full. A meter may borrow unused capacity. To make
  room, the queue drops the oldest message of the meter furthest above its
  quota; if no meter is above quota, it drops the globally oldest message.

### Per-meter priority (`buffer_priority`)

With a whitelist, every meter defaults to an equal-sized slice of the
buffer. `buffer_priority` lets some meters get a bigger slice than others -
for example a meter in a stairwell with poor signal that needs more retries
worth of headroom, versus one right next to the antenna:

```yaml
wmbus_radio:
  # ...
  forward_meters: [12345678, 87654321, "0x417F0666"]
  buffer_priority:
    "12345678": 3   # gets 3x the slice of a default-weight meter
    "87654321": 1   # same as leaving it unset
    # "0x417F0666" not listed -> also defaults to weight 1
```

**Why plain weights, not percentages.** A percentage-per-meter scheme has an
obvious failure mode: what happens when the percentages the user wrote do
not add up to 100? Either an error, or an implicit and confusing
renormalization. Plain positive integers avoid the question entirely - there
is no total they have to sum to. Each meter's exact share of the current
capacity is `capacity * weight / sum(all weights)`; the code floors every
share, then hands out the few leftover slots (`capacity` minus the sum of the
floors) one each to the meters with the largest fractional remainder. This is
the same *largest-remainder* (Hamilton) apportionment method used to allocate
parliamentary seats, and it has exactly the property this needs: quotas
always sum to **exactly** the current buffer capacity, for any positive
integer weights, with no rounding leftover and nothing for the user to get
wrong. A meter with no explicit entry defaults to weight 1, so leaving
`buffer_priority` unset entirely - or leaving one meter out of it - means an
equal split for that meter, not "gets nothing".

Quotas are recomputed whenever capacity changes (automatic checks every
~60 seconds, or a runtime edit through `buffer_capacity`). A recomputation
does not discard borrowed slots merely because a meter exceeds its quota:
only messages beyond the new global capacity are evicted. Quotas sum to
the current capacity and are logged at boot as
`MQTT outbox per-meter quotas (id:weight->slots)`.

`buffer_priority` without a whitelist has nothing to prioritise (there is no
fixed meter set to slice capacity across) and is ignored with a boot warning
if set that way.

### Sizing the buffer from free RAM (`mqtt_buffer_size: auto`)

The earlier fixed ceiling (256 frames) was picked as a round number, not
derived from what is actually free on the target board - a legitimate
concern, since a plain ESP32 (no PSRAM - e.g. the Olimex ESP32-POE this
project's own migration targets) only has a few hundred KB of heap total,
shared with WiFi/Ethernet/MQTT/TLS.

`mqtt_buffer_size: auto` replaces the fixed number with a value computed on
the device itself:

```yaml
wmbus_radio:
  # ...
  mqtt_buffer_size: auto
```

The calculation is implemented by `Radio::suggested_mqtt_outbox_capacity_()`
in `mqtt_outbox.cpp`:

- **Without PSRAM:** reserve 40 KB of internal heap, budget 25% of the free
  memory above that reserve, divide by an estimated 400 B per message, and
  clamp the result to 4–512 messages.
- **With PSRAM:** reserve 256 KB of PSRAM and 24 KB of internal heap. Budget
  50% of the remaining free PSRAM at 400 B per message and also limit by
  internal space at about 40 B per queue entry. Use the smaller result,
  clamped to 4–4096 messages. Topic/payload storage uses `RAMAllocator<char>`:
  PSRAM first, with internal-heap fallback; queue bookkeeping still uses
  internal memory.

These are estimates, not a guarantee that a particular capacity will fit.
The first calculation runs at startup and is checked again every ~60 seconds.
Changes within 15% are ignored unless the proposed ceiling is below the
current queue depth. An automatic resize updates both the ceiling and the
effective capacity; it can therefore replace a manual runtime capacity.

**Shrinking a full buffer:** messages beyond the new global capacity are
dropped immediately, oldest first without a whitelist, or using the
quota-based eviction above with a whitelist. Being over an individual quota
alone does not discard data. Automatic shrinkage logs the number of dropped
messages (`MQTT outbox auto-size shrink dropped ...`). Lowering the runtime
`buffer_capacity` below the current depth also trims immediately.

**The safety valve applies to both fixed and automatic capacities.** It
refuses queue growth if free internal heap falls below 40 KB without PSRAM,
or if free PSRAM falls below 256 KB or internal heap below 24 KB on a PSRAM
board. A warning is limited to once a minute. A nominal minimum capacity of
four does not override this protection: buffering may still be refused.

**Fragmentation is checked separately from total free heap.** A reserve is a
sum, and a sum says nothing about whether a single contiguous block is still
available - but WiFi, lwIP and esp-tls all need contiguous internal buffers
(a TLS record buffer alone is on the order of 16 KB). A long outage fills the
outbox with hundreds of small, variable-sized allocations, so on a board
without PSRAM free internal heap can still read comfortably above the 40 KB
reserve while the largest remaining block has dropped below what a reconnect
needs. The valve therefore also refuses once
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` falls under 16 KB,
logged as `MQTT outbox: internal heap too fragmented, refusing to buffer`.
Both figures are sampled on the same 1 Hz tick as the free-heap reads, for
the same reason: that call takes the heap lock the `radio_recv` task needs.
The check applies on the PSRAM path too, where only the ~40 B deque nodes are
internal and it should essentially never trigger - the argument for it does
not depend on where the payload bytes live.

## 2. Per-topic QoS

Every publish in this component used to be hardcoded: QoS 0 everywhere
except the `/rx` metadata topic, which was QoS 1. Five options now control
QoS per topic group, all defaulting to the previous hardcoded value so an
existing YAML with none of them set behaves exactly as before:

```yaml
wmbus_radio:
  # ...
  telegram_qos: 0      # wmbus/<topic>/telegram
  rssi_qos: 0           # wmbus/<topic>/rssi/<meter_id>
  health_qos: 0         # wmbus/<topic>/health and /meters
  diagnostic_qos: 0     # all wmbus/<topic>/diag/... topics (boot, config,
                         # summaries, suggestions, meter windows, drop/
                         # truncated events, busy_ether_changed)
  rx_qos: 1              # wmbus/<topic>/rx (rssi_dbm + received_at)
```

Per the migration spec's own QoS table (section 17/Table 26): QoS 1 is
recommended on the segments that actually need at-least-once delivery once a
consumer is idempotent (deduplicates on `raw_hash`/access number), and QoS 2
is not required anywhere in this system. A reasonable starting point once
the buffer above is enabled is `telegram_qos: 1` (matches `rx_qos`, which
was already 1); leave the rest at 0 unless a specific consumer needs more.

### Does QoS still apply when a message goes through the RAM buffer?

Yes, unchanged. `enqueue_or_publish_()` (the function every buffered publish
goes through - see part 1) takes the QoS value the caller already resolved
from `telegram_qos`/`rx_qos` and stores it *on the queued message itself*
(`OutboxMsg::qos`). When the message is later flushed - immediately if MQTT
is connected, or once it reconnects if it was queued - it is published with
that same stored QoS, not a fixed value. Buffering only changes *when* a
message reaches the broker, never *how* it is published. This required no
extra code to make correct: it falls straight out of storing the fully
resolved topic/payload/qos/retain of each message rather than a "buffer
everything at QoS 0" shortcut.

### Runtime QoS control (`select:` `telegram_qos` / `rx_qos`)

`telegram_qos`/`rx_qos` above set the *compiled starting value*. To raise
QoS for the two RAM-buffered topics from `0` to `1` or `2` live - without
reflashing - declare the optional `select:` sub-platform, on the same
lightweight authenticated `web_server:` portal the `buffer_capacity`
number: entity already uses:

```yaml
web_server:
  version: 3
  auth:
    username: !secret web_username
    password: !secret web_password

select:
  - platform: wmbus_radio
    wmbus_radio_id: radio_id_here     # the id: of your wmbus_radio: block
    telegram_qos:
      name: "WMBus Telegram QoS"
    rx_qos:
      name: "WMBus RX QoS"
```

Each entity is a dropdown with options `0`/`1`/`2`, starts at the value
`telegram_qos`/`rx_qos` compiled to, and calls straight into the same
`set_telegram_qos()`/`set_rx_qos()` setters the YAML options use - so a
change here is exactly as effective as reflashing with a different YAML
value, just without the reflash. It only affects **new** publishes from
that point on: a message already sitting in the RAM buffer keeps whichever
QoS it was queued with (see the answer above - QoS is captured per-message
at enqueue time).

## 3. The RX timestamp field: already there, next to RSSI

No code change was needed here. Every time a telegram is forwarded on
`wmbus/<topic>/telegram`, this component already publishes a companion JSON
message on `wmbus/<topic>/rx` (QoS controlled by `rx_qos` above) that
carries **both** fields together:

```json
{"schema":1,"boot_id":"...","seq":1,"rx_task_wakeup_us":...,
 "meter_id":"...","mode":"T1","rssi_dbm":-78,"frame_crc32":"...",
 "frame_length":32,"received_at":"2026-08-28T10:15:02.421Z"}
```

`received_at` is the wall-clock instant the frame was *received* (not
published — the two differ once buffering is involved), computed from
monotonic uptime at reception time. It is present only once SNTP has
synced; on a receiver that has not yet synced (the first seconds/minutes
after boot), the key is simply absent rather than carrying a false `1970`
or an uptime value disguised as a date — a consumer should treat a missing
key as "no reliable timestamp yet", not as an error.

Because the payload is built at reception time and only *enqueued or
published* afterwards, `received_at` stays accurate to the actual reception
instant even when the message spends time queued in the RAM buffer above.
