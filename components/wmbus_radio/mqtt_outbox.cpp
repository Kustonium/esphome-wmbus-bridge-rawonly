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

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"

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
static constexpr uint32_t AUTOSIZE_INTERVAL_MS = 30000;      // re-evaluate every ~30s in auto mode
static constexpr uint32_t HEAP_WARNING_THROTTLE_MS = 60000;  // at most one "buffer refused" warning per minute

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

static inline bool board_has_psram_() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

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
  if (suggested != this->mqtt_outbox_max_capacity_) {
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
             "MQTT outbox auto-size: %u -> %u frames (free heap=%u B, free psram=%u B) / "
             "auto-dobor bufora MQTT: %u -> %u ramek (wolny heap=%u B, wolny psram=%u B)",
             (unsigned) this->mqtt_outbox_max_capacity_, (unsigned) suggested,
             (unsigned) free_internal, (unsigned) free_psram,
             (unsigned) this->mqtt_outbox_max_capacity_, (unsigned) suggested,
             (unsigned) free_internal, (unsigned) free_psram);
    // If free heap has shrunk enough to lower the ceiling below what is
    // currently queued, the setters below trigger recompute_buffer_quotas_()
    // (see component.h), which trims the overflow immediately - oldest
    // messages first in shared-pool mode, or oldest-of-its-own-meter first
    // per meter when buffer_priority quotas are active. Nothing here waits
    // for the buffer to drain on its own; capturing dropped_total_ before/
    // after just makes that trim visible in the log instead of silent.
    const uint32_t dropped_before = this->mqtt_outbox_dropped_total_;
    this->set_mqtt_outbox_max_capacity(suggested);
    // Auto mode has no separate user-facing "target" to preserve independently
    // of the ceiling (unlike the number: entity in manual mode) - the ceiling
    // IS the current target, always.
    this->set_mqtt_outbox_capacity(suggested);
    if (this->buffer_capacity_number_ != nullptr) {
      this->buffer_capacity_number_->publish_state((float) this->mqtt_outbox_capacity_);
    }
    const uint32_t trimmed = this->mqtt_outbox_dropped_total_ - dropped_before;
    if (trimmed > 0) {
      ESP_LOGW(TAG,
               "MQTT outbox auto-size shrink dropped %u still-queued frame(s) to fit the new, smaller "
               "capacity (%u remaining queued) / zmniejszenie bufora MQTT (auto) odrzucilo %u "
               "oczekujacych ramek, aby zmiescic sie w nowej, mniejszej pojemnosci (%u pozostalo w kolejce)",
               (unsigned) trimmed, (unsigned) this->mqtt_outbox_.size(),
               (unsigned) trimmed, (unsigned) this->mqtt_outbox_.size());
    }
  }
}

void Radio::enqueue_or_publish_(const std::string &topic, const std::string &payload, uint8_t qos, bool retain,
                                 uint32_t meter_id, uint32_t meter_id_raw) {
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr) return;

  if (mqtt->is_connected()) {
    // Fast path, unchanged from before this feature existed. Also drains any
    // backlog first so messages stay in order: without this, a message that
    // enqueue_or_publish_ is asked to send *while* flush_mqtt_outbox_ is
    // mid-drain (same loop iteration) could jump ahead of older queued ones.
    if (!this->mqtt_outbox_.empty()) this->flush_mqtt_outbox_();
    if (this->mqtt_outbox_.empty()) {
      mqtt->publish(topic, payload, qos, retain);
      return;
    }
    // Flush could not fully drain this tick (see the cap in flush_mqtt_outbox_);
    // keep strict ordering by queuing behind what is still pending rather than
    // publishing out of turn.
  }

  // meter_bucket_key() tags whichever of meter_id/meter_id_raw is the real
  // identity for this meter (see meter_filter.h) - stable across BCD and
  // non-BCD meters, and consistent with how buffer_priority weights were
  // keyed when parsed in setup(). Computed up here so every drop path below
  // can attribute the loss to a meter (note_outbox_drop_).
  const uint64_t key = meter_bucket_key(meter_id, meter_id_raw);

  if (this->mqtt_outbox_capacity_ == 0) {
    // Buffering disabled (mqtt_buffer_size: 0, or lowered to 0 at runtime):
    // behave exactly like the upstream project today - drop silently.
    this->note_outbox_drop_(key, false);
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
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool refuse;
    const char *why;
    if (board_has_psram_()) {
      const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      refuse = free_psram < PSRAM_RESERVE_BYTES || free_internal < INTERNAL_RESERVE_WITH_PSRAM;
      why = "psram/heap below reserve";
    } else {
      refuse = free_internal < RAM_RESERVE_BYTES;
      why = "free heap below reserve";
    }
    if (refuse) {
      this->note_outbox_drop_(key, true);
      const uint32_t now_ms = (uint32_t) esphome::millis();
      if (this->last_outbox_heap_warning_ms_ == 0 ||
          (now_ms - this->last_outbox_heap_warning_ms_) >= HEAP_WARNING_THROTTLE_MS) {
        this->last_outbox_heap_warning_ms_ = now_ms;
        ESP_LOGW(TAG, "MQTT outbox: %s, refusing to buffer (free heap=%u B, free psram=%u B) / "
                      "odmowa buforowania: %s (wolny heap=%u B, wolny psram=%u B)",
                 why, (unsigned) free_internal, (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 why, (unsigned) free_internal, (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      }
      return;
    }
  }

  MeterQuota *mq = this->find_meter_quota_(key);

  if (mq != nullptr) {
    // Per-meter mode active (non-empty forward_meters whitelist): this
    // meter's own slice of the buffer, not the buffer as a whole, is what
    // has to make room. A noisy meter can then never crowd out a quiet one -
    // the noisy one just cycles through its own quota.
    if (mq->quota == 0) {
      // This meter's weight rounded down to zero slices of the current
      // capacity (more whitelisted meters than capacity allows). It gets no
      // buffer space at all rather than opportunistically borrowing room
      // that the next recompute would immediately reclaim from it anyway.
      this->note_outbox_drop_(key, false);
      return;
    }
    if (mq->count >= mq->quota) {
      // Full for this meter: drop ITS oldest queued message, not necessarily
      // the buffer's oldest overall, so other meters' quotas are untouched.
      for (auto it = this->mqtt_outbox_.begin(); it != this->mqtt_outbox_.end(); ++it) {
        if (it->meter_key == key) {
          this->mqtt_outbox_.erase(it);
          mq->count--;
          this->note_outbox_drop_(key, false);
          break;
        }
      }
    }
  } else {
    // Shared-pool mode (no whitelist, so no per-meter quotas exist): the
    // pre-existing single-FIFO behaviour, unchanged.
    if (this->mqtt_outbox_.size() >= this->mqtt_outbox_capacity_) {
      // Full: drop the oldest queued frame to make room for the newest one.
      // A long outage should keep the most recent readings, not lock onto
      // whatever was first in the window - the newest state is what a
      // reconnecting consumer needs most.
      const uint64_t evicted_key = this->mqtt_outbox_.front().meter_key;
      this->mqtt_outbox_.pop_front();
      this->note_outbox_drop_(evicted_key, false);
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
      this->note_outbox_drop_(key, true);
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
    ESP_LOGI(TAG, "MQTT outbox: buffering started, broker unreachable (1 frame queued) / "
                  "bufor MQTT: rozpoczeto buforowanie, broker nieosiagalny (1 ramka w kolejce)");
  } else {
    ESP_LOGD(TAG, "MQTT outbox: queued frame (%u in queue) / zakolejkowano ramke (%u w kolejce)",
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
    ESP_LOGI(TAG, "MQTT outbox: broker reachable again, draining %u queued frame(s) / "
                  "bufor MQTT: broker znow osiagalny, oproznianie %u ramek",
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
    ESP_LOGD(TAG, "MQTT outbox: flushed %u queued frame(s), %u still pending / bufor MQTT: wyslano %u ramek, w kolejce %u",
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
      const uint64_t evicted_key = this->mqtt_outbox_.front().meter_key;
      this->mqtt_outbox_.pop_front();
      this->note_outbox_drop_(evicted_key, false);
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
      const uint64_t evicted_key = this->mqtt_outbox_.front().meter_key;
      this->mqtt_outbox_.pop_front();
      this->note_outbox_drop_(evicted_key, false);
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

  // Re-derive live counts from what is actually queued right now - a
  // recompute can be triggered mid-flight by a runtime capacity change (auto
  // mode, or the buffer_capacity number entity), not just once at boot with
  // an empty queue.
  for (const auto &msg : this->mqtt_outbox_) {
    MeterQuota *mq = this->find_meter_quota_(msg.meter_key);
    if (mq != nullptr) mq->count++;
  }

  // Drop anything that no longer matches any whitelist entry at all (stale
  // messages from before a config change), then trim any meter that is now
  // over its (possibly just-shrunk) quota, oldest-of-that-meter first.
  for (auto it = this->mqtt_outbox_.begin(); it != this->mqtt_outbox_.end();) {
    if (this->find_meter_quota_(it->meter_key) == nullptr) {
      const uint64_t stale_key = it->meter_key;
      it = this->mqtt_outbox_.erase(it);
      this->note_outbox_drop_(stale_key, false);
    } else {
      ++it;
    }
  }
  for (auto &mq : this->mqtt_outbox_meter_quotas_) {
    while (mq.count > mq.quota) {
      bool erased = false;
      for (auto it = this->mqtt_outbox_.begin(); it != this->mqtt_outbox_.end(); ++it) {
        if (it->meter_key == mq.key) {
          this->mqtt_outbox_.erase(it);
          mq.count--;
          this->note_outbox_drop_(mq.key, false);
          erased = true;
          break;
        }
      }
      if (!erased) break;  // safety net: count/quota mismatch should not happen
    }
  }
}

void Radio::publish_initial_buffer_state_() {
  if (this->buffer_capacity_number_ != nullptr) {
    this->buffer_capacity_number_->publish_state((float) this->mqtt_outbox_capacity_);
  }
  // Compiled starting QoS for the optional runtime select: entities (see
  // component.h) - pushed here, not from Python codegen, so it reflects
  // telegram_qos_/rx_qos_ after the YAML setters have actually run,
  // regardless of to_code() call order between the main component and the
  // select: platform.
  if (this->telegram_qos_select_ != nullptr) {
    this->telegram_qos_select_->publish_state(std::to_string((unsigned) this->telegram_qos_));
  }
  if (this->rx_qos_select_ != nullptr) {
    this->rx_qos_select_->publish_state(std::to_string((unsigned) this->rx_qos_));
  }
}

// Every drop, from every path, goes through here: bumps the lifetime total,
// the per-outage total, the per-outage heap-refusal subset when applicable,
// and the per-outage per-meter breakdown used by the 30s stats line. The
// per-outage figures are zeroed by update_outbox_stats_ the moment the MQTT
// link drops.
void Radio::note_outbox_drop_(uint64_t meter_key, bool refused_heap) {
  this->mqtt_outbox_dropped_total_++;
  this->mqtt_outbox_dropped_this_outage_++;
  if (refused_heap) this->mqtt_outbox_refused_heap_this_outage_++;
  if (meter_key != 0) this->outbox_drop_by_meter_[meter_key]++;
}

void Radio::update_outbox_stats_(uint32_t now_ms) {
  // A new outage starts the instant the MQTT link goes down: reset the
  // per-outage drop accounting so buffer_dropped_last_outage and the 30s
  // stats breakdown always describe the current (or most recent) outage.
  // The lifetime mqtt_outbox_dropped_total_ is left alone.
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  const bool connected = (mqtt != nullptr && mqtt->is_connected());
  if (this->outbox_was_connected_ && !connected) {
    this->mqtt_outbox_dropped_this_outage_ = 0;
    this->mqtt_outbox_refused_heap_this_outage_ = 0;
    this->outbox_drop_by_meter_.clear();
  }
  this->outbox_was_connected_ = connected;

  // Gauges for an HA panel, not an event stream. Re-evaluated at most once a
  // second, and each sensor is only actually published when its value
  // changed - otherwise an idle bridge emitted identical sensor lines every
  // second forever.
  if (this->last_outbox_stats_ms_ != 0 && (now_ms - this->last_outbox_stats_ms_) < 1000) return;
  this->last_outbox_stats_ms_ = now_ms;

  if (this->buffer_dropped_last_outage_sensor_ != nullptr) {
    const float v = (float) this->mqtt_outbox_dropped_this_outage_;
    if (v != this->last_pub_dropped_outage_) {
      this->buffer_dropped_last_outage_sensor_->publish_state(v);
      this->last_pub_dropped_outage_ = v;
    }
  }
  if (this->buffer_depth_sensor_ != nullptr) {
    const float v = (float) this->mqtt_outbox_.size();
    if (v != this->last_pub_depth_) {
      this->buffer_depth_sensor_->publish_state(v);
      this->last_pub_depth_ = v;
    }
  }
  if (this->buffer_dropped_sensor_ != nullptr) {
    const float v = (float) this->mqtt_outbox_dropped_total_;
    if (v != this->last_pub_dropped_) {
      this->buffer_dropped_sensor_->publish_state(v);
      this->last_pub_dropped_ = v;
    }
  }
  if (this->buffer_oldest_age_sensor_ != nullptr) {
    float v = 0.0f;
    if (!this->mqtt_outbox_.empty()) {
      const uint32_t age_ms = now_ms - this->mqtt_outbox_.front().enqueued_ms;  // unsigned: still correct across the ~49-day millis() wrap
      v = (float) age_ms / 1000.0f;
    }
    // While a backlog exists this genuinely changes every tick (that is the
    // point); once drained it settles at 0 and stops publishing.
    if (v != this->last_pub_oldest_age_) {
      this->buffer_oldest_age_sensor_->publish_state(v);
      this->last_pub_oldest_age_ = v;
    }
  }

  // Once every 30s, one INFO line with the running buffer figures - but only
  // while it is worth reading: something queued now, or the lifetime drop
  // count moved since the last line. A healthy, idle bridge stays silent.
  if (this->last_outbox_stats_log_ms_ == 0 || (now_ms - this->last_outbox_stats_log_ms_) >= 30000) {
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
    }
  }
}

}  // namespace wmbus_radio
}  // namespace esphome
