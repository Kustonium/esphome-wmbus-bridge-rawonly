// SPDX-License-Identifier: GPL-3.0-or-later
//
// RAM store-and-forward for MQTT publishes.
//
// Why this exists: the project's own docs are explicit that when the local
// broker is unreachable, RF reception continues but every MQTT publish is
// simply skipped - the telegram is gone, not delayed. The RAM/flash buffer
// is called out as a "future feature" in the risk table. This is that
// feature, scoped to RAM only (no flash wear, no filesystem dependency), and
// scoped to the two channels where losing an event actually matters: the raw
// telegram itself and its /rx metadata companion (rssi_dbm + received_at).
//
// What is deliberately NOT buffered, and why:
//   - the retained per-meter RSSI scalar (wmbus/<topic>/rssi/<meter_id>):
//     it represents "latest known", not an event stream. Queuing a stale
//     value would risk it landing AFTER a fresher one published from a
//     later frame, which is worse than just skipping it - the next real
//     frame republishes a fresh value anyway.
//   - health/meters pulses, diagnostic summaries, suggestions, boot/config
//     snapshots: periodic or retained-liveness signals where a missed
//     window is meaningless once the next one arrives.
//   - the target-meter debug topic and the wmbus_bridge/raw dev tap: both
//     best-effort debugging aids, not the data path this feature protects.
//
// The buffer holds fully-serialized messages (topic/payload/qos/retain)
// rather than Frame objects. That keeps this file radio-agnostic and lets
// every call site decide independently whether to route through it.

#include "component.h"
#include "meter_filter.h"

#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/mqtt/mqtt_client.h"

// The sensor/number/select entities are OPTIONAL platforms of this component.
// Including their headers unconditionally forced AUTO_LOAD = ["sensor",
// "number", "select"], i.e. every user compiled three extra core components
// whether or not they ever declared one - which contradicts the point of the
// feature being opt-in. ESPHome defines USE_SENSOR/USE_NUMBER/USE_SELECT for
// whichever of those components a config actually loads, and declaring a
// `sensor: - platform: wmbus_radio` block loads the sensor component by
// itself, so guarding here is enough and AUTO_LOAD goes back to [].
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif

#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "wmbus";

// ── RAM sizing for the outbox ───────────────────────────────────────────────
//
// Why a fixed "256" ceiling was the wrong default: it was picked without
// looking at what is actually free on the target board. On a plain ESP32
// (no PSRAM - e.g. the Olimex ESP32-POE this project's own migration targets
// use) the whole heap is on the order of a few hundred KB, and WiFi/Ethernet/
// MQTT/TLS already claim a meaningful share of it before this component gets
// a say. A queue sized off "what sounds reasonable" instead of "what is
// actually spare" can starve those, not the outbox itself.
//
// EST_BYTES_PER_QUEUED_MSG is a deliberately generous, approximate estimate
// per queued OutboxMsg: std::string heap allocations for topic+payload
// (typically tens to a few hundred bytes for a telegram, ~200-380 bytes for
// the /rx metadata JSON) plus allocator/deque-node overhead. It is NOT a
// precise accounting - it exists to turn "how much free RAM can this safely
// use" into "how many messages" via one conservative division, not to model
// the allocator exactly.
//
// PSRAM note: heap_caps_get_free_size(MALLOC_CAP_SPIRAM) is read and logged
// for visibility (worth knowing on boards that have it), but the sizing
// formula below only budgets from INTERNAL heap. std::string on ESP-IDF
// allocates from the default (internal) heap allocator regardless of PSRAM
// presence, so a queued message does not actually spend PSRAM today; basing
// the suggestion on PSRAM would overstate how big the buffer can safely get.
// Routing outbox storage through a PSRAM allocator is a reasonable follow-up
// if larger buffers turn out to be needed on PSRAM boards, but is out of
// scope here.
static constexpr size_t RAM_RESERVE_BYTES = 40 * 1024;       // never eat into the last 40 KB of internal heap
static constexpr float OUTBOX_HEAP_BUDGET_FRACTION = 0.25f;  // at most 25% of what's free above the reserve
static constexpr size_t EST_BYTES_PER_QUEUED_MSG = 400;      // conservative average, see note above
static constexpr size_t MIN_SUGGESTED_CAPACITY = 4;
static constexpr size_t MAX_SUGGESTED_CAPACITY = 512;  // sanity cap when the buffer lives in internal heap
static constexpr uint32_t AUTOSIZE_INTERVAL_MS = 60000;      // re-evaluate every ~60s in auto mode
static constexpr uint32_t HEAP_WARNING_THROTTLE_MS = 60000;  // at most one "buffer refused" warning per minute
// How often the free-heap safety valve actually reads the heap. See the note
// at its call site in enqueue_or_publish_(): reading it per message was 2-4
// heap-lock acquisitions per received telegram for the whole length of an
// outage, on the exact hot path the radio task competes for.
static constexpr uint32_t HEAP_SAMPLE_INTERVAL_MS = 1000;
// Fragmentation floor for the safety valve, checked on BOTH the PSRAM and the
// internal-heap path.
//
// Why total free heap is not enough: the reserves above are sums, and a sum
// says nothing about whether a single contiguous block is still available.
// WiFi, lwIP and esp-tls all need contiguous internal buffers (a TLS record
// buffer alone is on the order of 16 KB), while a long outage fills the outbox
// with hundreds of small, variable-sized topic/payload allocations. On a board
// WITHOUT PSRAM those allocations are the outbox's payload bytes themselves,
// so free_internal can still read comfortably above RAM_RESERVE_BYTES while the
// largest remaining block has dropped below what a reconnect needs - the
// classic "plenty of free heap, yet WiFi will not come back up" failure. On a
// PSRAM board only the ~40 B deque nodes are internal, so this should
// essentially never trigger; it is checked there anyway because the argument
// for it does not depend on where the payload bytes live.
//
// Refusing to buffer is the soft failure (some frames dropped, one throttled
// warning explaining why); losing the network stack is the hard one.
static constexpr size_t INTERNAL_LARGEST_BLOCK_FLOOR_BYTES = 16 * 1024;
// Only actually resize (and pay for recompute_buffer_quotas_) when the new
// suggestion differs from the current ceiling by more than this fraction -
// otherwise free-heap jitter of a few KB flaps the capacity, and the number
// entity, every interval for no practical gain.
static constexpr float AUTOSIZE_HYSTERESIS_FRACTION = 0.15f;

// PSRAM-backed buffer sizing (used only when the board actually has PSRAM;
// RAMAllocator<char> then places topic+payload bytes there instead of internal
// heap). PSRAM is roomy, so the caps are much higher, but the reserve still
// protects other PSRAM users and the internal-heap reserve still applies to
// the small per-message deque node.
static constexpr size_t PSRAM_RESERVE_BYTES = 256 * 1024;          // keep this much PSRAM free for everything else
static constexpr float OUTBOX_PSRAM_BUDGET_FRACTION = 0.5f;        // at most 50% of free PSRAM above the reserve
static constexpr size_t INTERNAL_RESERVE_WITH_PSRAM = 24 * 1024;   // smaller internal margin: only deque nodes are internal now
static constexpr size_t EST_DEQUE_NODE_BYTES = 40;                 // OutboxMsg + deque bookkeeping, internal heap
static constexpr size_t MAX_SUGGESTED_CAPACITY_PSRAM = 4096;       // ~1.6 MB of payloads at EST_BYTES_PER_QUEUED_MSG

// Whether this SoC has usable PSRAM. Resolved once - heap_caps_get_total_size()
// takes the heap lock, and the radio_recv task can block on that same lock
// while doing a malloc mid-reception, so it must not be called every loop.
static bool board_has_psram_() {
  static const bool has = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
  return has;
}

// Low 32 bits of a meter_bucket_key = the printable meter id, i.e. the value
// the receive log shows as id:XXXXXXXX (raw A-field, or the BCD id for a
// meter that has no raw form). Used for the per-meter drop breakdown.
static inline uint32_t outbox_display_id_(uint64_t meter_key) { return (uint32_t) (meter_key & 0xFFFFFFFFu); }

size_t Radio::suggested_mqtt_outbox_capacity_() const {
  const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  if (board_has_psram_()) {
    // Payload bytes go to PSRAM; only the deque nodes cost internal heap.
    // The suggestion is the lower of "what fits in the PSRAM budget" and
    // "what fits in internal heap as deque nodes", so neither pool is
    // pushed past its reserve.
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= PSRAM_RESERVE_BYTES || free_internal <= INTERNAL_RESERVE_WITH_PSRAM)
      return MIN_SUGGESTED_CAPACITY;
    const size_t psram_budget = (size_t) ((free_psram - PSRAM_RESERVE_BYTES) * OUTBOX_PSRAM_BUDGET_FRACTION);
    const size_t by_psram = psram_budget / EST_BYTES_PER_QUEUED_MSG;
    const size_t by_internal = (free_internal - INTERNAL_RESERVE_WITH_PSRAM) / EST_DEQUE_NODE_BYTES;
    size_t suggested = by_psram < by_internal ? by_psram : by_internal;
    if (suggested < MIN_SUGGESTED_CAPACITY) suggested = MIN_SUGGESTED_CAPACITY;
    if (suggested > MAX_SUGGESTED_CAPACITY_PSRAM) suggested = MAX_SUGGESTED_CAPACITY_PSRAM;
    return suggested;
  }

  // No PSRAM: everything is in internal heap, exactly as before this option.
  if (free_internal <= RAM_RESERVE_BYTES) return MIN_SUGGESTED_CAPACITY;
  const size_t budget_bytes = (size_t) ((free_internal - RAM_RESERVE_BYTES) * OUTBOX_HEAP_BUDGET_FRACTION);
  size_t suggested = budget_bytes / EST_BYTES_PER_QUEUED_MSG;
  if (suggested < MIN_SUGGESTED_CAPACITY) suggested = MIN_SUGGESTED_CAPACITY;
  if (suggested > MAX_SUGGESTED_CAPACITY) suggested = MAX_SUGGESTED_CAPACITY;
  return suggested;
}

void Radio::maybe_reautosize_outbox_(uint32_t now_ms) {
  if (!this->mqtt_outbox_auto_) return;
  if (this->last_outbox_autosize_ms_ != 0 && (now_ms - this->last_outbox_autosize_ms_) < AUTOSIZE_INTERVAL_MS) return;
  this->last_outbox_autosize_ms_ = now_ms;

  const size_t suggested = this->suggested_mqtt_outbox_capacity_();
  const size_t current = this->mqtt_outbox_max_capacity_;
  const size_t delta = (suggested > current) ? (suggested - current) : (current - suggested);
  // Resize only on a meaningful change, or when the ceiling must drop below
  // what is queued right now (a real overflow that has to be trimmed). Small
  // free-heap jitter is ignored - that flap was firing recompute_buffer_quotas_
  // (twice) every interval, taking the heap lock the radio_recv task needs.
  const bool must_shrink = suggested < this->mqtt_outbox_.size();
  if ((float) delta > (float) current * AUTOSIZE_HYSTERESIS_FRACTION || must_shrink) {
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
             "MQTT outbox auto-size: %u -> %u messages (free heap=%u B, free psram=%u B) / "
             "auto-dobor bufora MQTT: %u -> %u wiadomosci (wolny heap=%u B, wolny psram=%u B)",
             (unsigned) current, (unsigned) suggested, (unsigned) free_internal, (unsigned) free_psram,
             (unsigned) current, (unsigned) suggested, (unsigned) free_internal, (unsigned) free_psram);
    const uint32_t dropped_before = this->mqtt_outbox_dropped_total_;
    // Set both members, then recompute ONCE (the two setters would each call
    // recompute_buffer_quotas_ - wasteful, and it is the recompute that costs).
    this->mqtt_outbox_max_capacity_ = suggested;
    this->mqtt_outbox_capacity_ = suggested;
    if (this->setup_done_)
      this->recompute_buffer_quotas_();
#ifdef USE_NUMBER
    if (this->buffer_capacity_number_ != nullptr) {
      this->buffer_capacity_number_->publish_state((float) this->mqtt_outbox_capacity_);
    }
#endif
    const uint32_t trimmed = this->mqtt_outbox_dropped_total_ - dropped_before;
    if (trimmed > 0) {
      ESP_LOGW(TAG,
               "MQTT outbox auto-size shrink dropped %u still-queued message(s) to fit the new, smaller "
               "capacity (%u remaining queued) / zmniejszenie bufora MQTT (auto) odrzucilo %u "
               "oczekujacych wiadomosci, aby zmiescic sie w nowej, mniejszej pojemnosci (%u pozostalo w kolejce)",
               (unsigned) trimmed, (unsigned) this->mqtt_outbox_.size(),
               (unsigned) trimmed, (unsigned) this->mqtt_outbox_.size());
    }
  }
}

void Radio::enqueue_or_publish_(const std::string &topic, const std::string &payload, uint8_t qos, bool retain,
                                 uint32_t meter_id, uint32_t meter_id_raw) {
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr) return;

  const bool connected = mqtt->is_connected();
  if (connected) {
    // Fast path, unchanged from before this feature existed. Also drains any
    // backlog first so messages stay in order: without this, a message that
    // enqueue_or_publish_ is asked to send *while* flush_mqtt_outbox_ is
    // mid-drain (same loop iteration) could jump ahead of older queued ones.
    if (!this->mqtt_outbox_.empty()) this->flush_mqtt_outbox_();
    if (this->mqtt_outbox_.empty()) {
      // Return value CHECKED. publish() reports whether the message was
      // actually handed to esp-mqtt; discarding that verdict lost the
      // telegram outright in the one window this whole feature exists to
      // cover: the broker is already gone but ESPHome has not yet missed a
      // PING_RESP, so is_connected() still says true (documented in this
      // PR's own "MQTT detection lag" limitation). flush_mqtt_outbox_()
      // below already checks the same return value on the drain path;
      // ignoring it here was the asymmetry. On failure we fall through and
      // queue the message instead of dropping it.
      if (mqtt->publish(topic, payload, qos, retain))
        return;
    }
    // Flush could not fully drain this tick (see the cap in flush_mqtt_outbox_);
    // keep strict ordering by queuing behind what is still pending rather than
    // publishing out of turn.
  }

  // Notice the start of an outage HERE, on the frame path, not only from the
  // 1 Hz sampler in update_outbox_stats_. The sampler resets the per-outage
  // counters when it observes the connected->disconnected edge, so every
  // drop counted during the up-to-one-second gap between "broker gone" and
  // "sampler noticed" was being zeroed a moment later - losing exactly the
  // beginning of the outage, which is the interesting part. Called before
  // any note_outbox_drop_() below so this call's own drops survive.
  this->note_outbox_link_state_(connected);

  // A frame carries BOTH forms of its id at runtime: meter_id (BCD-decoded,
  // non-zero for a BCD-able A-field) and meter_id_raw (the raw A-field). A
  // buffer_priority / forward_meters entry was keyed with only ONE of them,
  // whichever the YAML token implied (hex -> raw, decimal -> BCD). So resolve
  // the quota by trying BOTH key forms and canonicalise msg.meter_key to the
  // one the quota actually uses - otherwise the keys never match, the quota
  // is bypassed, and recompute_buffer_quotas_ treats every queued frame as
  // "not whitelisted" and wipes the buffer on its next 30s pass.
  const uint64_t key_bcd = (meter_id != 0) ? meter_bucket_key(meter_id, 0) : 0;
  const uint64_t key_raw = (meter_id_raw != 0) ? meter_bucket_key(0, meter_id_raw) : 0;
  const uint32_t display_id = (meter_id_raw != 0) ? meter_id_raw : meter_id;
  uint64_t key = key_bcd != 0 ? key_bcd : key_raw;  // fallback identity when no quota matches
  MeterQuota *mq = nullptr;
  if (key_raw != 0 && (mq = this->find_meter_quota_(key_raw)) != nullptr) {
    key = key_raw;
  } else if (key_bcd != 0 && (mq = this->find_meter_quota_(key_bcd)) != nullptr) {
    key = key_bcd;
  }

  if (this->mqtt_outbox_capacity_ == 0) {
    // Buffering disabled (mqtt_buffer_size: 0, or lowered to 0 at runtime):
    // behave exactly like the upstream project today - drop silently.
    this->note_outbox_drop_(display_id, false);
    return;
  }

  // Hard backstop, independent of mqtt_outbox_capacity_: even a manually
  // chosen mqtt_buffer_size that looked safe at boot must not be allowed to
  // starve WiFi/MQTT/TLS of RAM later (more diagnostics enabled, unusually
  // large payloads, heap fragmentation over uptime, ...). Applies
  // unconditionally. On a PSRAM board the payload bytes are in PSRAM, so the
  // limiting reserve is PSRAM's (plus a small internal margin for the deque
  // node); without PSRAM it is the internal-heap reserve, as before.
  {
    // Free heap is SAMPLED, not read per message. heap_caps_get_free_size()
    // takes the heap lock, and the radio_recv task can block on that same
    // lock while doing a malloc mid-reception - which is exactly why
    // board_has_psram_() above is memoised. Reading it on every enqueue
    // undid that: 2 reads per message here plus 2 more in the warning path,
    // and every telegram enqueues TWO messages (itself + its /rx
    // companion), so a busy outage meant up to 8 heap-lock acquisitions per
    // received telegram, sustained. On a single-core part (ESP32-S2/C3,
    // where this was tested) that is the margin between "FIFO drained in
    // time" and "RX FIFO overflow". One sample per second is just as
    // protective: at ~400 B per queued message the buffer cannot take free
    // heap from "above reserve" to "dangerously low" inside one second.
    const uint32_t heap_now_ms = (uint32_t) esphome::millis();
    if (this->last_heap_sample_ms_ == 0 ||
        (heap_now_ms - this->last_heap_sample_ms_) >= HEAP_SAMPLE_INTERVAL_MS) {
      this->last_heap_sample_ms_ = heap_now_ms;
      this->cached_free_internal_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      this->cached_free_psram_ = board_has_psram_() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
      // Same 1 Hz tick on purpose: heap_caps_get_largest_free_block() takes
      // the same heap lock, so reading it per message would undo exactly the
      // contention fix the sampling above exists for.
      this->cached_largest_internal_ = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    }
    const size_t free_internal = this->cached_free_internal_;
    const size_t free_psram = this->cached_free_psram_;
    const size_t largest_internal = this->cached_largest_internal_;
    // Checked on both paths - see INTERNAL_LARGEST_BLOCK_FLOOR_BYTES.
    const bool internal_fragmented = largest_internal < INTERNAL_LARGEST_BLOCK_FLOOR_BYTES;
    bool refuse;
    const char *why;
    if (board_has_psram_()) {
      const bool below_reserve = free_psram < PSRAM_RESERVE_BYTES || free_internal < INTERNAL_RESERVE_WITH_PSRAM;
      refuse = below_reserve || internal_fragmented;
      why = below_reserve ? "psram/heap below reserve" : "internal heap too fragmented";
    } else {
      const bool below_reserve = free_internal < RAM_RESERVE_BYTES;
      refuse = below_reserve || internal_fragmented;
      why = below_reserve ? "free heap below reserve" : "internal heap too fragmented";
    }
    if (refuse) {
      this->note_outbox_drop_(display_id, true);
      if (this->last_outbox_heap_warning_ms_ == 0 ||
          (heap_now_ms - this->last_outbox_heap_warning_ms_) >= HEAP_WARNING_THROTTLE_MS) {
        this->last_outbox_heap_warning_ms_ = heap_now_ms;
        ESP_LOGW(TAG,
                 "MQTT outbox: %s, refusing to buffer (free heap=%u B, largest block=%u B, free psram=%u B) / "
                 "odmowa buforowania: %s (wolny heap=%u B, najwiekszy blok=%u B, wolny psram=%u B)",
                 why, (unsigned) free_internal, (unsigned) largest_internal, (unsigned) free_psram,
                 why, (unsigned) free_internal, (unsigned) largest_internal, (unsigned) free_psram);
      }
      return;
    }
  }

  if (mq != nullptr) {
    // Per-meter mode active (non-empty forward_meters whitelist). Quotas are
    // LAZY: a meter may use as much of the buffer as is free, exceeding its
    // buffer_priority slice, and nothing is dropped for that. The quota only
    // decides who loses a frame when the WHOLE buffer is actually full - then
    // the meter most over its fair share is trimmed, oldest first, so a quiet
    // meter's arriving frame always finds room. If the outage clears while
    // there is still space, the "over-quota" data was never thrown away.
    if (this->mqtt_outbox_.size() >= this->mqtt_outbox_capacity_) {
      this->evict_one_for_priority_();
    }
  } else {
    // Shared-pool mode (no whitelist, so no per-meter quotas exist): the
    // pre-existing single-FIFO behaviour, unchanged.
    if (this->mqtt_outbox_.size() >= this->mqtt_outbox_capacity_) {
      // Full: drop the oldest queued frame to make room for the newest one.
      // A long outage should keep the most recent readings, not lock onto
      // whatever was first in the window - the newest state is what a
      // reconnecting consumer needs most.
      const uint32_t evicted_id = outbox_display_id_(this->mqtt_outbox_.front().meter_key);
      this->mqtt_outbox_.pop_front();
      this->note_outbox_drop_(evicted_id, false);
    }
  }

  OutboxMsg msg;
  {
    // PSRAM first, internal-heap fallback (see OutboxMsg doc in component.h).
    RAMAllocator<char> alloc;
    const size_t tlen = topic.size();
    const size_t plen = payload.size();
    msg.topic = alloc.allocate(tlen + 1);
    char *pbuf = (plen > 0) ? alloc.allocate(plen) : nullptr;
    if (msg.topic == nullptr || (plen > 0 && pbuf == nullptr)) {
      // Out of room even after the fallback - same outcome as the reserve
      // check above: refuse, count it as a heap refusal. msg's destructor
      // frees whatever partially allocated.
      this->note_outbox_drop_(display_id, true);
      return;
    }
    memcpy(msg.topic, topic.data(), tlen);
    msg.topic[tlen] = '\0';
    if (pbuf != nullptr) {
      memcpy(pbuf, payload.data(), plen);
      msg.payload = pbuf;
      msg.payload_len = (uint16_t) plen;
    }
  }
  msg.qos = qos;
  msg.retain = retain;
  msg.enqueued_ms = (uint32_t) esphome::millis();
  msg.meter_key = key;
  this->mqtt_outbox_.push_back(std::move(msg));
  this->mqtt_outbox_queued_total_++;
  if (mq != nullptr) mq->count++;

  // One INFO line when a backlog first starts (broker just went unreachable);
  // every further frame during the same outage is DEBUG so a long outage does
  // not flood the log. The periodic "MQTT outbox stats" line (every 30s, see
  // update_outbox_stats_) carries the running depth.
  if (this->mqtt_outbox_.size() == 1) {
    ESP_LOGI(TAG, "MQTT outbox: buffering started, broker unreachable (1 message queued) / "
                  "bufor MQTT: rozpoczeto buforowanie, broker nieosiagalny (1 wiadomosc w kolejce)");
  } else {
    ESP_LOGD(TAG, "MQTT outbox: queued message (%u in queue) / zakolejkowano wiadomosc (%u w kolejce)",
             (unsigned) this->mqtt_outbox_.size(), (unsigned) this->mqtt_outbox_.size());
  }
}

void Radio::flush_mqtt_outbox_() {
  if (this->mqtt_outbox_.empty()) {
    if (this->outbox_draining_) {
      this->outbox_draining_ = false;
      ESP_LOGI(TAG, "MQTT outbox: backlog cleared / bufor MQTT: kolejka oprozniona");
    }
    return;
  }
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  // One INFO line when draining actually begins (broker came back); the
  // per-batch progress below is DEBUG so a large backlog does not flood.
  if (!this->outbox_draining_) {
    this->outbox_draining_ = true;
    ESP_LOGI(TAG, "MQTT outbox: broker reachable again, draining %u queued message(s) / "
                  "bufor MQTT: broker znow osiagalny, oproznianie %u wiadomosci",
             (unsigned) this->mqtt_outbox_.size(), (unsigned) this->mqtt_outbox_.size());
  }

  // Bounded per call: a receiver that was offline for hours can wake up with
  // thousands of queued frames, and publishing all of them in one loop()
  // iteration would starve the radio receiver task and the rest of loop()
  // for a long stretch. Draining a bounded slice per tick spreads that cost
  // out; the backlog empties over the following ticks instead of in one.
  static constexpr size_t MAX_FLUSH_PER_CALL = 8;
  size_t sent = 0;
  while (!this->mqtt_outbox_.empty() && sent < MAX_FLUSH_PER_CALL) {
    const OutboxMsg &msg = this->mqtt_outbox_.front();
    // Length-explicit overload: msg.payload is a raw char buffer, not a
    // NUL-terminated string. msg.topic is NUL-terminated.
    if (!mqtt->publish(std::string(msg.topic), msg.payload != nullptr ? msg.payload : "",
                       (size_t) msg.payload_len, msg.qos, msg.retain)) {
      // Publish failed (e.g. connection dropped mid-flush): stop and retry
      // next loop() rather than dropping the message.
      break;
    }
    // Release this message's slot in its meter's quota before popping it -
    // find_meter_quota_ needs the key, which pop_front() would discard.
    MeterQuota *mq = this->find_meter_quota_(msg.meter_key);
    if (mq != nullptr && mq->count > 0) mq->count--;
    this->mqtt_outbox_.pop_front();
    sent++;
  }
  if (sent > 0) {
    ESP_LOGD(TAG, "MQTT outbox: flushed %u queued message(s), %u still pending / bufor MQTT: wyslano %u wiadomosci, w kolejce %u",
             (unsigned) sent, (unsigned) this->mqtt_outbox_.size(), (unsigned) sent, (unsigned) this->mqtt_outbox_.size());
  }
  if (this->mqtt_outbox_.empty() && this->outbox_draining_) {
    this->outbox_draining_ = false;
    ESP_LOGI(TAG, "MQTT outbox: backlog cleared / bufor MQTT: kolejka oprozniona");
  }
}

Radio::MeterQuota *Radio::find_meter_quota_(uint64_t key) {
  for (auto &mq : this->mqtt_outbox_meter_quotas_) {
    if (mq.key == key) return &mq;
  }
  return nullptr;
}

// Per-meter buffer_priority quotas.
//
// Problem this solves: with a forward_meters whitelist, one noisy meter
// (or one that goes quiet on the broker side longer than others) should not
// be able to fill the whole buffer and crowd out a whitelisted meter that
// only sends a telegram every few minutes. Splitting mqtt_outbox_capacity_
// into a fixed slice per meter fixes that; buffer_priority just lets some
// meters get a bigger slice than others.
//
// Why largest-remainder (Hamilton) apportionment instead of asking for
// percentages: the user explicitly did not want a scheme where a config
// mistake (weights, or percentages, not summing to the "right" total) leaves
// the buffer mis-sized or partly unused. Plain integer weights sidestep that
// entirely - there is no total they have to sum to. Each meter's exact share
// is capacity * weight / sum(weights); flooring every share and then handing
// the few leftover slots (capacity minus the sum of the floors) to the
// entries with the largest fractional remainder is a standard apportionment
// method (the same idea used to allocate parliamentary seats) and it has the
// property this needs: the quotas always sum to EXACTLY mqtt_outbox_capacity_,
// for any positive integer weights, with no rounding leftover anywhere.
//
// A meter in the whitelist with no explicit buffer_priority entry defaults to
// weight 1 (see set_buffer_priority_csv() in component.h), so leaving
// buffer_priority unset entirely means an equal split - not "undefined" or
// "gets nothing".
void Radio::recompute_buffer_quotas_() {
  if (this->forward_meter_ids_.empty() && this->forward_meter_raw_ids_.empty()) {
    // Shared-pool mode: no whitelist configured, so there is no fixed meter
    // set to slice capacity across. Behaves exactly as before this feature
    // existed - one flat FIFO, trimmed from the front when it grows past
    // capacity (e.g. after the capacity number entity is lowered at runtime).
    this->mqtt_outbox_meter_quotas_.clear();
    while (this->mqtt_outbox_.size() > this->mqtt_outbox_capacity_) {
      const uint32_t evicted_id = outbox_display_id_(this->mqtt_outbox_.front().meter_key);
      this->mqtt_outbox_.pop_front();
      this->note_outbox_drop_(evicted_id, false);
    }
    return;
  }

  struct Entry {
    uint64_t key;
    uint32_t weight;
    double exact;
    size_t quota;
  };
  std::vector<Entry> entries;
  entries.reserve(this->forward_meter_ids_.size() + this->forward_meter_raw_ids_.size());
  for (uint32_t id : this->forward_meter_ids_) {
    const uint64_t key = meter_bucket_key(id, 0);
    auto it = this->buffer_priority_weights_.find(key);
    const uint32_t w = (it != this->buffer_priority_weights_.end()) ? it->second : 1;
    entries.push_back(Entry{key, w, 0.0, 0});
  }
  for (uint32_t raw : this->forward_meter_raw_ids_) {
    const uint64_t key = meter_bucket_key(0, raw);
    auto it = this->buffer_priority_weights_.find(key);
    const uint32_t w = (it != this->buffer_priority_weights_.end()) ? it->second : 1;
    entries.push_back(Entry{key, w, 0.0, 0});
  }

  uint64_t total_weight = 0;
  for (const auto &e : entries) total_weight += e.weight;
  if (entries.empty() || total_weight == 0) {
    // Should not normally happen (every whitelist entry defaults to weight
    // 1), but fall back to shared-pool behaviour rather than dividing by
    // zero if it ever does.
    this->mqtt_outbox_meter_quotas_.clear();
    while (this->mqtt_outbox_.size() > this->mqtt_outbox_capacity_) {
      const uint32_t evicted_id = outbox_display_id_(this->mqtt_outbox_.front().meter_key);
      this->mqtt_outbox_.pop_front();
      this->note_outbox_drop_(evicted_id, false);
    }
    return;
  }

  const size_t capacity = this->mqtt_outbox_capacity_;
  size_t assigned = 0;
  for (auto &e : entries) {
    e.exact = (double) capacity * (double) e.weight / (double) total_weight;
    e.quota = (size_t) e.exact;  // floor
    assigned += e.quota;
  }
  // Hand out the capacity - assigned leftover slots to the largest
  // fractional remainders first, so the quotas sum to exactly `capacity`.
  size_t leftover = capacity - assigned;
  std::vector<size_t> order(entries.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::sort(order.begin(), order.end(), [&entries](size_t a, size_t b) {
    const double ra = entries[a].exact - (double) entries[a].quota;
    const double rb = entries[b].exact - (double) entries[b].quota;
    if (ra != rb) return ra > rb;
    return entries[a].key < entries[b].key;  // deterministic tie-break
  });
  for (size_t i = 0; i < leftover && i < order.size(); i++) entries[order[i]].quota++;

  this->mqtt_outbox_meter_quotas_.clear();
  this->mqtt_outbox_meter_quotas_.reserve(entries.size());
  for (const auto &e : entries) {
    MeterQuota mq;
    mq.key = e.key;
    mq.weight = e.weight;
    mq.quota = e.quota;
    mq.count = 0;
    this->mqtt_outbox_meter_quotas_.push_back(mq);
  }

  // Nothing queued (the normal case - a recompute fired only because the
  // capacity moved): the fresh quota vector above is all that was needed.
  if (this->mqtt_outbox_.empty())
    return;

  // One O(n) pass: drop only frames whose meter matches no quota at all
  // (stale after a config change) - those genuinely no longer belong.
  {
    // Filtered IN PLACE. Building a second std::deque and swapping temporarily
    // doubled the deque's node storage in INTERNAL heap - and this runs
    // precisely when the buffer is fullest, i.e. when internal heap is
    // tightest. At the auto-sized ~1760 messages measured on a 2 MB-PSRAM
    // board that second deque is roughly 56 KB against an
    // INTERNAL_RESERVE_WITH_PSRAM of 24 KB, so the allocation could fail (or
    // push internal heap under its reserve) in the middle of an outage - the
    // one moment the buffer must not misbehave. deque::erase() is O(n) in the
    // worst case but allocates nothing, and this loop already was O(n).
    for (auto it = this->mqtt_outbox_.begin(); it != this->mqtt_outbox_.end();) {
      if (this->find_meter_quota_(it->meter_key) != nullptr) {
        ++it;
      } else {
        this->note_outbox_drop_(outbox_display_id_(it->meter_key), false);
        it = this->mqtt_outbox_.erase(it);
      }
    }
  }

  // Re-derive live per-meter counts from what survived.
  for (auto &mq : this->mqtt_outbox_meter_quotas_)
    mq.count = 0;
  for (const auto &msg : this->mqtt_outbox_) {
    MeterQuota *mq = this->find_meter_quota_(msg.meter_key);
    if (mq != nullptr)
      mq->count++;
  }

  // Lazy quotas: enforce only the GLOBAL cap here. A capacity drop while the
  // buffer is not actually full trims NOTHING - a meter over its quota keeps
  // its frames until a competing frame needs the room (handled in
  // enqueue_or_publish_). This is what stops a shrink from throwing away
  // e.g. 60 real readings just to reserve empty slots for meters that may
  // not transmit for minutes.
  while (this->mqtt_outbox_.size() > this->mqtt_outbox_capacity_)
    this->evict_one_for_priority_();
}

// KNOWN LIMITATION, stated here because every eviction path goes through this
// function or the shared-pool pop_front above it: a telegram and its /rx
// metadata companion are two INDEPENDENT entries in the queue. Eviction makes
// room for one message, so it can drop the telegram and leave its companion
// queued (or the reverse). The backend then receives rssi_dbm/received_at for
// a telegram that never arrives, or a telegram with no metadata - not a crash
// or a lost reading, but a row it cannot correlate.
//
// Not fixed here on purpose: the clean fix is a pair id stamped on both
// messages when the frame is forwarded (maybe_forward_frame_ in
// mqtt_publish.cpp), with eviction removing every entry carrying that id, and
// that is a design change to the queue's contents rather than a bug fix - it
// deserves its own commit and its own hardware run, not a quiet edit inside a
// review.
void Radio::evict_one_for_priority_() {
  if (this->mqtt_outbox_.empty())
    return;

  // Victim meter: the one furthest over its quota (largest count - quota).
  MeterQuota *victim = nullptr;
  long worst_over = 0;
  for (auto &mq : this->mqtt_outbox_meter_quotas_) {
    const long over = (long) mq.count - (long) mq.quota;
    if (over > worst_over) {
      worst_over = over;
      victim = &mq;
    }
  }

  auto it = this->mqtt_outbox_.begin();  // default: globally oldest
  if (victim != nullptr) {
    for (auto scan = this->mqtt_outbox_.begin(); scan != this->mqtt_outbox_.end(); ++scan) {
      if (this->find_meter_quota_(scan->meter_key) == victim) {
        it = scan;  // oldest frame of the over-quota meter
        break;
      }
    }
  }

  const uint32_t drop_id = outbox_display_id_(it->meter_key);
  MeterQuota *mq = this->find_meter_quota_(it->meter_key);
  if (mq != nullptr && mq->count > 0)
    mq->count--;
  this->mqtt_outbox_.erase(it);
  this->note_outbox_drop_(drop_id, false);
}

void Radio::publish_initial_buffer_state_() {
#ifdef USE_NUMBER
  if (this->buffer_capacity_number_ != nullptr) {
    this->buffer_capacity_number_->publish_state((float) this->mqtt_outbox_capacity_);
  }
#endif
  // Compiled starting QoS for the optional runtime select: entities (see
  // component.h) - pushed here, not from Python codegen, so it reflects
  // telegram_qos_/rx_qos_ after the YAML setters have actually run,
  // regardless of to_code() call order between the main component and the
  // select: platform.
#ifdef USE_SELECT
  if (this->telegram_qos_select_ != nullptr) {
    this->telegram_qos_select_->publish_state(std::to_string((unsigned) this->telegram_qos_));
  }
  if (this->rx_qos_select_ != nullptr) {
    this->rx_qos_select_->publish_state(std::to_string((unsigned) this->rx_qos_));
  }
#endif
}

// Every drop, from every path, goes through here: bumps the lifetime total,
// the per-outage total, the per-outage heap-refusal subset when applicable,
// and the per-outage per-meter breakdown used by the 30s stats line. The
// per-outage figures are zeroed by update_outbox_stats_ the moment the MQTT
// link drops.
// Single place that decides "a new outage has begun". Called from both the
// 1 Hz sampler (update_outbox_stats_) and the frame path (enqueue_or_publish_),
// so whichever notices the disconnect first does the reset and the other one
// is a no-op. Idempotent by construction: the reset only fires on the
// connected->disconnected transition.
void Radio::note_outbox_link_state_(bool connected) {
  if (this->outbox_was_connected_ && !connected) {
    this->mqtt_outbox_dropped_this_outage_ = 0;
    this->mqtt_outbox_refused_heap_this_outage_ = 0;
    this->outbox_drop_by_meter_.clear();
  }
  this->outbox_was_connected_ = connected;
}

void Radio::note_outbox_drop_(uint32_t display_id, bool refused_heap) {
  this->mqtt_outbox_dropped_total_++;
  this->mqtt_outbox_dropped_this_outage_++;
  if (refused_heap) this->mqtt_outbox_refused_heap_this_outage_++;
  if (display_id != 0) this->outbox_drop_by_meter_[display_id]++;
}

void Radio::update_outbox_stats_(uint32_t now_ms) {
  // Gauges for an HA panel, not an event stream - everything here (including
  // the MQTT link-state check) is gated to at most once a second so it stays
  // out of the way of the radio_recv task.
  if (this->last_outbox_stats_ms_ != 0 && (now_ms - this->last_outbox_stats_ms_) < 1000) return;
  this->last_outbox_stats_ms_ = now_ms;

  // A new outage starts the instant the MQTT link goes down: reset the
  // per-outage drop accounting so buffer_dropped_last_outage and the 30s
  // stats breakdown always describe the current (or most recent) outage.
  // The lifetime mqtt_outbox_dropped_total_ is left alone. 1 s granularity
  // is fine - the outbox itself reacts to the disconnect on its own path.
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  const bool connected = (mqtt != nullptr && mqtt->is_connected());
  this->note_outbox_link_state_(connected);

  const float depth = (float) this->mqtt_outbox_.size();
  const float dropped_total = (float) this->mqtt_outbox_dropped_total_;
  const float dropped_outage = (float) this->mqtt_outbox_dropped_this_outage_;
  float oldest_age = 0.0f;
  if (!this->mqtt_outbox_.empty()) {
    const uint32_t age_ms = now_ms - this->mqtt_outbox_.front().enqueued_ms;  // unsigned: still correct across the ~49-day millis() wrap
    oldest_age = (float) age_ms / 1000.0f;
  }

  const bool tick30 =
      (this->last_outbox_stats_log_ms_ == 0 || (now_ms - this->last_outbox_stats_log_ms_) >= 30000);

  // Publish all four buffer sensors TOGETHER, and only when something moved
  // (a frame entered the queue, or drained, or was dropped) or on the 30s
  // tick. The oldest-age gauge would otherwise change - and re-publish - every
  // second for the whole length of an outage; here HA just sees it refreshed
  // alongside depth/drops and at least every 30s.
  const bool moved = depth != this->last_pub_depth_ || dropped_total != this->last_pub_dropped_ ||
                     dropped_outage != this->last_pub_dropped_outage_;
  if (moved || tick30) {
#ifdef USE_SENSOR
    if (this->buffer_depth_sensor_ != nullptr && depth != this->last_pub_depth_)
      this->buffer_depth_sensor_->publish_state(depth);
    if (this->buffer_dropped_sensor_ != nullptr && dropped_total != this->last_pub_dropped_)
      this->buffer_dropped_sensor_->publish_state(dropped_total);
    if (this->buffer_dropped_last_outage_sensor_ != nullptr && dropped_outage != this->last_pub_dropped_outage_)
      this->buffer_dropped_last_outage_sensor_->publish_state(dropped_outage);
    if (this->buffer_oldest_age_sensor_ != nullptr && oldest_age != this->last_pub_oldest_age_)
      this->buffer_oldest_age_sensor_->publish_state(oldest_age);
#endif
    this->last_pub_depth_ = depth;
    this->last_pub_dropped_ = dropped_total;
    this->last_pub_dropped_outage_ = dropped_outage;
    this->last_pub_oldest_age_ = oldest_age;
  }

  // Once every 30s, one INFO line with the running buffer figures - but only
  // while it is worth reading: something queued now, or the lifetime drop
  // count moved since the last line. A healthy, idle bridge stays silent.
  if (tick30) {
    this->last_outbox_stats_log_ms_ = now_ms;
    if (!this->mqtt_outbox_.empty() || this->mqtt_outbox_dropped_total_ != this->last_stats_log_dropped_) {
      this->last_stats_log_dropped_ = this->mqtt_outbox_dropped_total_;
      const uint32_t evicted_outage =
          this->mqtt_outbox_dropped_this_outage_ - this->mqtt_outbox_refused_heap_this_outage_;
      const char *mode = board_has_psram_() ? (this->mqtt_outbox_auto_ ? " auto,psram" : " psram")
                                            : (this->mqtt_outbox_auto_ ? " auto" : "");
      ESP_LOGI(TAG,
               "MQTT outbox stats: depth=%u cap=%u/%u%s | dropped total=%u this-outage=%u "
               "(heap-refused=%u evicted=%u) | queued total=%u / statystyki bufora MQTT: kolejka=%u "
               "poj=%u/%u%s | odrzucone total=%u ta-awaria=%u (brak-RAM=%u eksmisja=%u) | zakolejkowane=%u",
               (unsigned) this->mqtt_outbox_.size(), (unsigned) this->mqtt_outbox_capacity_,
               (unsigned) this->mqtt_outbox_max_capacity_, mode,
               (unsigned) this->mqtt_outbox_dropped_total_, (unsigned) this->mqtt_outbox_dropped_this_outage_,
               (unsigned) this->mqtt_outbox_refused_heap_this_outage_, (unsigned) evicted_outage,
               (unsigned) this->mqtt_outbox_queued_total_,
               (unsigned) this->mqtt_outbox_.size(), (unsigned) this->mqtt_outbox_capacity_,
               (unsigned) this->mqtt_outbox_max_capacity_, mode,
               (unsigned) this->mqtt_outbox_dropped_total_, (unsigned) this->mqtt_outbox_dropped_this_outage_,
               (unsigned) this->mqtt_outbox_refused_heap_this_outage_, (unsigned) evicted_outage,
               (unsigned) this->mqtt_outbox_queued_total_);

      // Per-meter drop breakdown for this outage - shows whether buffer_priority
      // is actually protecting the meters it should. Printed as id=count using
      // the same id form the receive log uses (id:XXXXXXXX).
      if (!this->outbox_drop_by_meter_.empty()) {
        std::string by_meter;
        for (const auto &kv : this->outbox_drop_by_meter_) {
          char b[40];
          snprintf(b, sizeof(b), "%s%08X=%u", by_meter.empty() ? "" : " ",
                   (unsigned) (kv.first & 0xFFFFFFFFu), (unsigned) kv.second);
          by_meter += b;
        }
        ESP_LOGI(TAG, "MQTT outbox drops this outage by meter / odrzucone w tej awarii wg licznika: %s",
                 by_meter.c_str());
      }

      // Per-meter occupancy: how many of each whitelisted meter's frames are
      // queued right now vs. the slice buffer_priority gives it (count/quota).
      // Only meaningful with a forward_meters whitelist (otherwise the vector
      // is empty and it is one shared FIFO). Cheap: count/quota are already
      // maintained on the quota vector, this just formats them.
      if (!this->mqtt_outbox_meter_quotas_.empty()) {
        std::string per_meter;
        for (const auto &mq : this->mqtt_outbox_meter_quotas_) {
          char b[40];
          snprintf(b, sizeof(b), "%s%08X=%u/%u", per_meter.empty() ? "" : " ",
                   (unsigned) (mq.key & 0xFFFFFFFFu), (unsigned) mq.count, (unsigned) mq.quota);
          per_meter += b;
        }
        ESP_LOGI(TAG, "MQTT outbox per-meter queued/quota / w buforze wg licznika (kolejka/limit): %s",
                 per_meter.c_str());
      }
    }
  }
}

}  // namespace wmbus_radio
}  // namespace esphome
