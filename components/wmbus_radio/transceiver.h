// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esphome/components/spi/spi.h"
#include "esphome/core/optional.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <string>

#define BYTE(x, n) ((uint8_t)(x >> (n * 8)))

namespace esphome {
namespace wmbus_radio {
enum ListenMode : uint8_t { LISTEN_MODE_BOTH = 0, LISTEN_MODE_T1 = 1, LISTEN_MODE_C1 = 2, LISTEN_MODE_S1 = 3 };

// How long a notify wait blocks before the caller gives up on more bytes.
//
// Never set this to one tick. A FreeRTOS block time is counted in ticks and
// expires at the *next* tick interrupt, not a full period later: a one-tick
// wait issued just before that interrupt returns almost immediately. At
// CONFIG_FREERTOS_HZ=1000 (what ESPHome sets) "1 ms" is therefore a random
// draw from 0-1 ms, and the give-up path fires on tick phase rather than on
// the radio having nothing to give. Two ticks guarantee one whole period.
//
// The byte loops below do not depend on this - each driver's read() first
// polls the FIFO against a hardware-timer deadline, so this wait is the second
// line, not the first. It is still the last place where a give-up could be
// decided by a coin flip, which is why it is named and stated once here.
// Diagnosis credit: IoTLabs-pl/esphome-components 72e76be (2026-08-05), which
// hit the same quantization as a receive regression.
static constexpr uint32_t WMBUS_NOTIFY_WAIT_MS = 2;
class RadioTransceiver
    : public Component,
      public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                            spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_2MHZ> {
public:
  virtual void setup() override = 0;
  void dump_config() override;

  template <typename T>
  void attach_data_interrupt(void (*callback)(T *), T *arg) {
    this->irq_pin_->attach_interrupt(callback, arg, this->irq_edge_);
  }
  virtual void restart_rx() = 0;
  virtual int8_t get_rssi() = 0;
  virtual const char *get_name() = 0;

  // No transmit API on purpose. This component is a receiver: it listens,
  // validates and republishes. A TX test path existed briefly (added 0ba1df5,
  // removed 6318cd4) and left behind config fields and a virtual that no
  // driver implemented, which read as a working feature to anyone browsing the
  // source. If transmitting is ever needed, it belongs in a separate project,
  // not behind an undocumented switch here.

  // Radio-specific recovery hints for the upper RX pipeline.
  // Default: keep the generic strict path.
  virtual bool supports_preamble_retry() const { return false; }
  virtual bool supports_unknown_size_raw_drain() const { return false; }

  // Live RSSI of the channel right now, as opposed to get_rssi(), which every
  // driver caches from the last packet capture. The two are easy to confuse and
  // the difference matters: a noise-floor measurement built on the cached value
  // would only ever see signals that were received, which is the very feedback
  // loop it exists to escape.
  //
  // Returns false when the radio cannot answer; the caller must then simply not
  // sample, rather than fall back to something that looks like an answer.
  virtual bool read_channel_rssi_dbm(int8_t *out) { return false; }
  // Optional SX1276-style early abort hooks / diagnostics.
  virtual bool supports_weak_partial_start_abort() const { return false; }
  virtual bool consume_rx_abort_request() { return false; }
  virtual uint32_t take_fifo_overrun_count() { return 0; }

  // Optional radio-specific debug dump. Used when RX waits time out.
  virtual void dump_debug_status(const char *reason) {}

  // Verbose diagnostics, pushed down from the component before the receiver
  // task starts.
  //
  // A driver's provenance snapshots (see RssiDiag) are one per receive path per
  // boot by default, which is the right volume for a healthy node: it answers
  // "where did this level come from" once and then stops narrating. It is the
  // wrong volume when a node is receiving nothing and every capture is
  // evidence - one sample cannot separate a real frame captured out of
  // alignment from a detector firing on noise. With this on, drivers report
  // every capture instead.
  void set_diag_verbose(bool enabled) { this->diag_verbose_ = enabled; }
  bool diag_verbose() const { return this->diag_verbose_; }

  // Optional register dump logged once at boot (called from component boot log).
  virtual void log_reg_status() {}

  // Optional: report SX126x device errors captured during boot clear.
  // Default: not supported.
  virtual bool get_boot_device_errors(uint16_t &before, uint16_t &after) const { return false; }

  // Where the RSSI attached to a received frame came from.
  //
  // A driver can compute a frame's level from several radio registers, and when
  // that level looks wrong it matters a great deal which one was used. The
  // receive path cannot report this itself: it runs in the receiver task, and
  // log output from that task does not reach the API log stream - only UART.
  // So the driver records a snapshot and Radio::loop() drains it below, which
  // puts the message on the main task where it is actually visible.
  // The two string members are never null, so a half-filled snapshot cannot
  // turn into a null %s at the log call.
  struct RssiDiag {
    const char *path{"?"};    // receive path that produced the frame
    const char *source{"?"};  // radio register the level was taken from
    uint8_t raw_sync{0};      // raw RssiSync (0 = not latched)
    uint8_t raw_avg{0};       // raw RssiAvg (0 = not latched)
    int8_t inflight{0};       // level sampled while the frame was on air
    int8_t result{0};         // level finally reported for the frame
    uint16_t trigger_irq{0};  // IRQ snapshot that selected the receive path
    uint16_t captured{0};     // bytes captured before the upper drain limit
    const char *exit_reason{"n/a"};
    uint8_t first_bytes[8]{};
    uint8_t first_len{0};
  };

  // Pops one pending snapshot. Returns false when there is nothing left to
  // report, which is the default for drivers that do not record provenance.
  // Call it until it returns false: a driver may have more than one waiting.
  virtual bool take_rssi_diag(RssiDiag &out) { return false; }

  bool read_in_task(uint8_t *buffer, size_t length);
  bool read_in_task_partial(uint8_t *buffer, size_t max_length, size_t &out_read,
                            uint32_t wait_ms = WMBUS_NOTIFY_WAIT_MS, uint8_t idle_rounds = 1);

  void set_spi(spi::SPIDelegate *spi);
  void set_reset_pin(InternalGPIOPin *reset_pin);
  void set_irq_pin(InternalGPIOPin *irq_pin);
  void set_busy_pin(InternalGPIOPin *busy_pin);
  void set_listen_mode(ListenMode mode) { this->listen_mode_ = mode; }
  ListenMode get_listen_mode() const { return this->listen_mode_; }
  const std::string &get_rf_params_str() const { return this->rf_params_str_; }

protected:
  InternalGPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  InternalGPIOPin *busy_pin_{nullptr};

  // SX127x DIO for FIFO level is typically active-low (falling edge).
  // SX126x DIO for IRQ is active-high (rising edge).
  gpio::InterruptType irq_edge_{gpio::INTERRUPT_FALLING_EDGE};

  ListenMode listen_mode_{LISTEN_MODE_BOTH};
  std::string rf_params_str_{};
  bool diag_verbose_{false};

  virtual optional<uint8_t> read() = 0;

  void reset();
  void common_setup();
  uint8_t spi_transaction(uint8_t operation, uint8_t address,
                          std::initializer_list<uint8_t> data);
  uint8_t spi_read(uint8_t address);
  void spi_write(uint8_t address, std::initializer_list<uint8_t> data);
  void spi_write(uint8_t address, uint8_t data);
};

} // namespace wmbus_radio
} // namespace esphome
