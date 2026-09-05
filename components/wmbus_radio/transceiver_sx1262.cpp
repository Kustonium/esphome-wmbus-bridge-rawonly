// SPDX-License-Identifier: GPL-3.0-or-later
#include "transceiver_sx1262.h"
#include "decode3of6.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <algorithm>

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "SX1262";

// ---------------------------------------------------------------------------
// SX126x SPI command opcodes (subset used for WMBus RX)
// Reference: SX1261/2 datasheet Rev 2.2, Table 11-1
// ---------------------------------------------------------------------------
static constexpr uint8_t CMD_SET_STANDBY = 0x80;
static constexpr uint8_t CMD_SET_PACKET_TYPE = 0x8A;
static constexpr uint8_t CMD_SET_RF_FREQUENCY = 0x86;
static constexpr uint8_t CMD_SET_BUFFER_BASE_ADDRESS = 0x8F;
static constexpr uint8_t CMD_SET_MODULATION_PARAMS = 0x8B;
static constexpr uint8_t CMD_SET_PACKET_PARAMS = 0x8C;
static constexpr uint8_t CMD_SET_DIO_IRQ_PARAMS = 0x08;
static constexpr uint8_t CMD_SET_RX = 0x82;
static constexpr uint8_t CMD_GET_IRQ_STATUS = 0x12;
static constexpr uint8_t CMD_CLEAR_IRQ_STATUS = 0x02;
static constexpr uint8_t CMD_GET_RX_BUFFER_STATUS = 0x13;
static constexpr uint8_t CMD_READ_BUFFER = 0x1E;
static constexpr uint8_t CMD_GET_PACKET_STATUS = 0x14;
static constexpr uint8_t CMD_GET_RSSI_INST = 0x15;
static constexpr uint8_t CMD_GET_DEVICE_ERRORS = 0x17;
static constexpr uint8_t CMD_CLEAR_DEVICE_ERRORS = 0x07;
static constexpr uint8_t CMD_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D;
static constexpr uint8_t CMD_SET_DIO3_AS_TCXO_CTRL = 0x97;
static constexpr uint8_t CMD_GET_STATUS = 0xC0;
static constexpr uint8_t CMD_CALIBRATE = 0x89;
static constexpr uint8_t CMD_CALIBRATE_IMAGE = 0x98;
static constexpr uint8_t CMD_WRITE_REGISTER = 0x0D;
static constexpr uint8_t CMD_READ_REGISTER = 0x1D;

// ---------------------------------------------------------------------------
// SX126x standby / packet type constants
// STANDBY_RC   - internal RC oscillator, used after reset and before XOSC is ready
// STANDBY_XOSC - crystal oscillator, required before entering RX/TX
// ---------------------------------------------------------------------------
static constexpr uint8_t STANDBY_RC = 0x00;
static constexpr uint8_t STANDBY_XOSC = 0x01;

static constexpr uint8_t PACKET_TYPE_GFSK = 0x00;

// ---------------------------------------------------------------------------
// GFSK modulation / packet parameter constants
// ---------------------------------------------------------------------------
static constexpr uint8_t GFSK_PULSE_SHAPE_NONE  = 0x00;  // no filter — upstream (Szczepan) and the LR1121 driver both use this
static constexpr uint8_t GFSK_PULSE_SHAPE_BT_0_5 = 0x09;
static constexpr uint8_t GFSK_RX_BW_312_0 = 0x19;
static constexpr uint8_t GFSK_RX_BW_234_3 = 0x0A;  // 234.3 kHz RX bandwidth (C1)
static constexpr uint8_t GFSK_RX_BW_156_2 = 0x1A;  // 156.2 kHz RX bandwidth (S1 sweep point)
static constexpr uint8_t GFSK_PREAMBLE_DETECT_8  = 0x04;  // detect after 8 preamble bits — more sensitive, tolerates noisy/weak preamble starts
static constexpr uint8_t GFSK_PREAMBLE_DETECT_16 = 0x05;  // detect after 16 preamble bits (previous default)
static constexpr uint8_t GFSK_ADDRESS_FILT_OFF = 0x00;

// S1 matches the full 24-bit S-mode sync word (54 76 96); T1/C1 use 16 bits.
//
// Measured 2026-07-31, worth not repeating: shortening this to 16 bits was tried
// to find out whether weak S-mode frames were failing to trigger SYNC_WORD_VALID
// at all - on this driver the entire S1 path starts at that interrupt, so a
// detector that never fires and an antenna that hears nothing both count zero.
// With 16 bits the detector did start firing, but on noise: captures ran to the
// 512-byte cap with first bytes that decode to no valid L/C, while the SX1276
// used as reference received nothing at all in the same minutes. The shorter
// word buys false starts, not missed meters. An 18-bit native-sync experiment
// on 2026-08-14 behaved the same way: with no S1 transmitter it repeatedly
// triggered on noise and filled the 256-byte capture buffer. Keep 24 bits.
static constexpr uint8_t S1_SYNC_WORD_BITS = 24;
#define S1_SYNC_WORD_BITS_OR_DEFAULT(mode) \
  static_cast<uint8_t>((mode) == LISTEN_MODE_S1 ? S1_SYNC_WORD_BITS : 16)

// SX126x GFSK header type:
// 0x00 = FIX_LEN (no length byte in-air)
// 0x01 = VAR_LEN (first payload byte is length in-air)
// WMBus does NOT carry a "GFSK length byte" at the start of the payload, so we must use FIX_LEN.
static constexpr uint8_t GFSK_PACKET_FIX_LEN = 0x00;
static constexpr uint8_t GFSK_PACKET_VAR_LEN = 0x01;

static constexpr uint8_t GFSK_CRC_OFF = 0x01;
static constexpr uint8_t GFSK_WHITENING_OFF = 0x00;

// Calibrate(calibParam) bit mask, SX1261/2 datasheet Rev 2.2 table 13-4:
// bit0 RC64k, bit1 RC13M, bit2 PLL, bit3 ADC pulse, bit4 ADC bulk N,
// bit5 ADC bulk P, bit6 image. 0x7F therefore recalibrates every block.
static constexpr uint8_t CALIBRATE_ALL = 0x7F;

// Device-error bits (GetDeviceErrors), same datasheet. Only the two that say
// "the reference or the PLL never came up" are named; the rest are TX-side.
static constexpr uint16_t DEV_ERR_PLL_CALIB = 0x0010;
static constexpr uint16_t DEV_ERR_XOSC_START = 0x0020;

// ---------------------------------------------------------------------------
// IRQ flag bits (GetIrqStatus register)
// ---------------------------------------------------------------------------
static constexpr uint16_t IRQ_RX_DONE = 0x0002;
// IRQ_SYNC_WORD_VALID: sync word matched - used as long-packet start trigger
static constexpr uint16_t IRQ_SYNC_WORD_VALID = 0x0008;

static uint16_t u16be_(const uint8_t *p) { return (uint16_t(p[0]) << 8) | uint16_t(p[1]); }
static constexpr uint16_t IRQ_TIMEOUT = 0x0200;   // RxTxTimeout
static constexpr uint16_t IRQ_CRC_ERROR = 0x0040; // CRC error

// ---------------------------------------------------------------------------
// Register addresses (SX1261/2 datasheet Rev 2.2)
// ---------------------------------------------------------------------------
// Sync word base register
static constexpr uint16_t REG_SYNC_WORD_0 = 0x06C0;

// Rx gain register (datasheet SX1261/2 Rev 2.2, "Rx Gain" / boosted-RX section).
// 0x94 = power-saving gain (chip default after reset), 0x96 = boosted gain:
// better sensitivity at a higher RX current. Not retained over sleep, so it is
// re-written on every setup. These two byte values come straight from the
// datasheet table — they are not tuned here.
static constexpr uint16_t REG_RX_GAIN = 0x08AC;
static constexpr uint8_t RX_GAIN_POWER_SAVING = 0x94;
static constexpr uint8_t RX_GAIN_BOOSTED = 0x96;

// RSSI/AGC calibration registers (datasheet Rev 2.2, section 6.1.6). Their
// boot values are logged for the Heltec V4 + GC1109 experiment; do not tune
// them without a calibrated RF source.
static constexpr uint16_t REG_AGC_RSSI_MEAS_CAL_H = 0x089C;
static constexpr uint16_t REG_AGC_RSSI_MEAS_CAL_L = 0x089D;
static constexpr uint16_t REG_AGC_GAIN_TUNE_1_2 = 0x08F5;
static constexpr uint16_t REG_AGC_GAIN_TUNE_3_4 = 0x08F6;
static constexpr uint16_t REG_AGC_GAIN_TUNE_5_6 = 0x08F7;
static constexpr uint16_t REG_AGC_GAIN_TUNE_7_8 = 0x08F8;
static constexpr uint16_t REG_AGC_GAIN_TUNE_9_10 = 0x08F9;
static constexpr uint16_t REG_AGC_GAIN_TUNE_11_12 = 0x08FA;
static constexpr uint16_t REG_AGC_GAIN_TUNE_13 = 0x08FB;
static constexpr uint16_t REG_AGC_FIRST_POWER_THRESHOLD = 0x08B9;

// GFSK baseline registers - NOT in the SX1261/2 datasheet at all. Source:
// RadioLib's SX126x::fixGFSK() (modules/SX126x/SX126x.cpp), whose own comment
// says these come from "Semtech repositories", not the public datasheet.
// RadioLib applies one set of values for 600/1200 bps and a different
// "reset" set for every other bitrate - T1 (100 kbps) and S1 (32768 bps)
// both fall in the second group, so both use the same four values below.
// 0x08B8 sits one address below the AGC FirstPow register above; same
// undocumented calibration neighbourhood, but these four are copied
// verbatim from a working reference implementation, not guessed.
static constexpr uint16_t REG_GFSK_FIX_1 = 0x06D1;
static constexpr uint16_t REG_GFSK_FIX_3 = 0x08B8;
static constexpr uint16_t REG_GFSK_FIX_4 = 0x06AC;
static constexpr uint16_t REG_RSSI_AVG_WINDOW = 0x089B;

// Semtech AN1200.53 long-packet reception registers:
//   REG_RX_ADDR_PTR      - current HW write pointer into the 256-byte circular buffer
//   REG_RXTX_PAYLOAD_LEN - intended end index; radio asserts RX_DONE when pointer reaches this value
static constexpr uint16_t REG_RX_ADDR_PTR = 0x0803;
static constexpr uint16_t REG_RXTX_PAYLOAD_LEN = 0x06BB;

// RF frequency step for SX126x: 32e6 / 2^25 (Hz)
static constexpr uint32_t XTAL_FREQ = 32000000UL;

static inline void u16_to_be(uint16_t v, uint8_t &msb, uint8_t &lsb) {
  msb = (uint8_t)((v >> 8) & 0xFF);
  lsb = (uint8_t)(v & 0xFF);
}

// ---------------------------------------------------------------------------
// RSSI sentinel, identical to the SX1276 driver: "not measured for this frame".
// Both consumers recognise it: the heuristics in rf_runtime.cpp bail out on it,
// and rssi_is_measured_() in component.cpp keeps it out of the diagnostic and
// per-meter averages. A frame whose level could not be sampled is therefore
// reported as unknown rather than as a bogus near-floor reading.
// ---------------------------------------------------------------------------
static constexpr int8_t RSSI_NOT_MEASURED = -127;

// Floor for a converted reading. This must stay *above* the whole "unusable"
// band, not just above the sentinel: rf_runtime.cpp bails out on
// `rssi_dbm <= -126` and component.cpp counts a sample as measured only when
// `rssi_dbm > -126`. Clamping to -126 would push a genuine (if meaningless)
// near-floor reading into that band and have it silently dropped, so the
// measurement range and the no-data band are kept disjoint by construction.
// Raw 250..255 maps to -125; those levels are below thermal noise anyway.
static constexpr int8_t RSSI_MIN_VALID = -125;

// Ceiling for a plausible reading, expressed as the raw register value.
//
// Raw 0 has always been treated as "register never written", but the check
// stopped there, and raw 1 converts to -0.5 dBm which integer division turns
// into a clean 0 dBm. A wM-Bus frame at 0 dBm does not exist: the SX126x
// front end saturates around -5 dBm, and even a transmitter on the same desk
// at minimum power lands tens of dB below that. Observed in the field on
// 2026-07-31 - a decoded frame logged as "RSSI: 0dBm" - which is worse than no
// reading at all, because a fabricated number gets reasoned about.
//
// Raw 20 = -10 dBm. Anything above that is rejected as unmeasured rather than
// reported; nothing legitimate is lost, since no real reception reaches it.
static constexpr uint8_t RSSI_RAW_MIN_PLAUSIBLE = 20;

// ---------------------------------------------------------------------------
// SX1261/2 datasheet Rev 2.2: RSSI [dBm] = -RssiRaw / 2.
//
// The old code spelled this as `-((int) raw) >> 1`. Unary minus binds tighter
// than the shift, so it arithmetic-shifted a negative value, which floors
// instead of truncating (raw=241 -> -121 dBm instead of -120.5 -> -120) and
// only accidentally agreed with the datasheet for even raw values.
//
// A raw value of 0 means "register never written", not "0 dBm"; callers must
// check for it before calling this.
// ---------------------------------------------------------------------------
static inline int8_t sx126x_rssi_dbm_(uint8_t raw) {
  // Reject the impossible end of the scale here rather than at each call site:
  // every caller already understands the sentinel, and one of them getting the
  // check wrong is how "RSSI: 0dBm" reached a decoded frame in the first place.
  if (raw < RSSI_RAW_MIN_PLAUSIBLE)
    return RSSI_NOT_MEASURED;
  const int dbm = -((int) raw) / 2;
  return (int8_t) (dbm < RSSI_MIN_VALID ? RSSI_MIN_VALID : dbm);
}

// Decode the S1 L- and C-fields from the first four raw Manchester bytes.
// The full parser tries both polarities as well; here we additionally require a
// valid wM-Bus C-field so the complemented polarity cannot select a plausible
// but wrong L-field. Returns the exact number of raw Manchester bytes needed
// ---------------------------------------------------------------------------
// Frame-start search, diagnostic only.
//
// s1_expected_raw_len_() below assumes the frame begins at chip 0 of the
// captured buffer, because the radio strips the sync word in hardware and the
// payload should start immediately after it. Every S1 capture on this driver
// ends at exit=buffer_cap, which is what happens when that assumption fails and
// no length can be derived - and three captures of the same repeater
// transmission, decoded identically by an SX1276 in the same second, produced
// three completely different first bytes here.
//
// So look instead of assuming: try every chip offset and both polarities, and
// report where a valid L+C actually sits. A stable answer of chip 0 clears the
// capture path; an answer that moves between captures locates the bug.
// ---------------------------------------------------------------------------

// One Manchester pair at absolute chip index `chip` in raw. Returns false when
// the pair is 00 or 11, which encodes nothing.
static bool s1_chip_pair_(const std::vector<uint8_t> &raw, size_t chip, bool polarity, uint8_t &bit_out) {
  const size_t a_i = chip * 2U;
  const size_t b_i = a_i + 1U;
  if ((b_i >> 3) >= raw.size())
    return false;
  const uint8_t a = (uint8_t) ((raw[a_i >> 3] >> (7U - (a_i & 7U))) & 0x01U);
  const uint8_t b = (uint8_t) ((raw[b_i >> 3] >> (7U - (b_i & 7U))) & 0x01U);
  if (a == b)
    return false;
  bit_out = (uint8_t) ((a == 0 && b == 1) ? 0 : 1);
  if (polarity)
    bit_out ^= 1U;
  return true;
}

// Decode `count` bytes starting at Manchester-pair index `start_pair`. Returns
// false on the first invalid pair.
static bool s1_decode_bytes_(const std::vector<uint8_t> &raw, size_t start_pair, bool polarity,
                             uint8_t *out, size_t count) {
  for (size_t i = 0; i < count; i++) {
    uint8_t byte = 0;
    for (size_t bit = 0; bit < 8; bit++) {
      uint8_t v = 0;
      if (!s1_chip_pair_(raw, start_pair + i * 8U + bit, polarity, v))
        return false;
      byte = (uint8_t) ((byte << 1U) | v);
    }
    out[i] = byte;
  }
  return true;
}

// How the invalid pairs of one candidate frame spread over its CRC blocks.
//
// A total on its own ("38 invalid pairs") cannot say whether resolving erasures
// against the CRC is worth building. An invalid Manchester pair is a *known*
// error position, so a block holding k of them is 2^k substitutions to try, and
// the blocks are checked independently: six erasures spread one per block are
// six 2-try problems, the same six inside one block are 64 tries. Only the
// distribution separates those, and it is not known which one real frames give.
//
// Format-A block layout, in decoded bytes: 10 data + 2 CRC, then 16 + 2 each,
// the last block holding whatever is left + 2. Decoded bit i comes from pair i,
// so decoded byte b spans pairs [b*8, b*8+8).
//
// Counted over the frame window only. `symbols_invalid` from the decode path is
// counted over the whole capture, so at buffer_cap it is mostly the noise after
// the frame - 246 B of noise contribute ~492 invalid pairs on their own, which
// is how a ratio that looks like signal quality ends up describing the tail.
static void s1_log_erasure_map_(const std::vector<uint8_t> &raw, size_t start_pair, bool polarity,
                                uint8_t l_field, unsigned candidate) {
  const size_t frame_len = (size_t) l_field + 1U;  // data bytes, L included, CRCs excluded

  // An L-field of 255 gives 17 blocks; 18 is the ceiling, not an expected value.
  uint16_t per_block[18]{};
  size_t block_end[18]{};  // exclusive, in decoded bytes, CRCs included
  size_t blocks = 0, decoded_bytes = 0, left = frame_len;
  while (left > 0 && blocks < 18) {
    const size_t data = (blocks == 0) ? 10U : 16U;
    const size_t take = (left < data) ? left : data;
    decoded_bytes += take + 2U;
    left -= take;
    block_end[blocks++] = decoded_bytes;
  }

  const size_t pairs = decoded_bytes * 8U;
  size_t counted = 0, total = 0, block = 0;
  uint16_t worst = 0;
  for (size_t p = 0; p < pairs; p++) {
    if (((start_pair + p) * 2U + 1U) >> 3 >= raw.size())
      break;  // capture ended inside the frame
    counted++;
    const size_t byte = p / 8U;
    while (block + 1U < blocks && byte >= block_end[block])
      block++;
    uint8_t v = 0;
    if (!s1_chip_pair_(raw, start_pair + p, polarity, v)) {
      per_block[block]++;
      total++;
      if (per_block[block] > worst)
        worst = per_block[block];
    }
  }

  char list[128];
  size_t off = 0;
  for (size_t i = 0; i < blocks && off + 8U < sizeof(list); i++) {
    const int n = snprintf(list + off, sizeof(list) - off, "%s%u", i ? "," : "",
                           (unsigned) per_block[i]);
    if (n <= 0)
      break;
    off += (size_t) n;
  }
  list[sizeof(list) - 1] = '\0';

  // The worst block sets the cost of the whole frame - blocks are resolved one
  // at a time, so it is 2^worst substitutions, not 2^total.
  ESP_LOGI(TAG,
           "S1 erasure map %u: %u erasures in %u/%u frame pairs, per CRC block [%s], "
           "worst block %u (2^%u tries)%s",
           candidate, (unsigned) total, (unsigned) counted, (unsigned) pairs, list,
           (unsigned) worst, (unsigned) worst, counted < pairs ? ", capture ended early" : "");
}

// Raw length of a complete format-A frame with DLL CRCs, from its L-field.
static size_t s1_raw_len_from_l_(uint8_t l_field) {
  const size_t frame_len = (size_t) l_field + 1U;
  const size_t blocks = (l_field < 26U) ? 2U : (size_t) ((l_field - 26U) / 16U + 3U);
  return (frame_len + 2U * blocks) * 2U;
}

void SX1262::log_s1_frame_start_(const std::vector<uint8_t> &raw) {
  // Longest unbroken run of valid Manchester pairs anywhere in the capture,
  // reported first because it answers the coarser question on its own.
  //
  // Pair validity (a != b) does not depend on polarity, only on whether the
  // chip grid is read on the even or the odd boundary - so two single passes
  // cover every possibility. An 85-byte telegram is 680 consecutive valid
  // pairs; noise breaks every few. A capture that holds the frame the SX1276
  // decoded in the same second cannot have a longest run in the tens.
  size_t best_run = 0, best_run_at = 0;
  for (uint8_t parity = 0; parity < 2; parity++) {
    size_t run = 0, run_start = 0;
    for (size_t chip = parity; ; chip++) {
      uint8_t v = 0;
      if ((chip * 2U + 1U) >> 3 >= raw.size())
        break;
      if (s1_chip_pair_(raw, chip, false, v)) {
        if (run == 0)
          run_start = chip;
        run++;
        if (run > best_run) {
          best_run = run;
          best_run_at = run_start;
        }
      } else {
        run = 0;
      }
    }
  }
  ESP_LOGI(TAG, "S1 capture quality: longest valid Manchester run %u pairs at chip %u (of %u pairs total)",
           (unsigned) best_run, (unsigned) best_run_at, (unsigned) ((raw.size() * 8U) / 2U));

  // Header search over EVERY chip position in the capture.
  //
  // Reported as a LIST, not as the first hit. The test - L in a sane range and
  // C in {0x44, 0x46} - passes on roughly 0.8% of positions by chance, so over
  // 4096 positions about 30 false hits are expected and the first one carries
  // no information. Measured 2026-08-01: the first hit was L=0x18 on polarity 1
  // while the frame an SX1276 decoded in the same second was L=0x54 on
  // polarity 0.
  //
  // Sorting by how clean the implied frame is puts a real header at the top,
  // because a real frame decodes with few invalid pairs and a coincidence
  // usually does not.
  struct Hit { size_t chip; uint8_t polarity; uint8_t l; uint8_t c; uint16_t invalid; uint16_t checked; };
  Hit hits[4]{};
  size_t hit_count = 0;

  const size_t max_start_pair = (raw.size() * 8U) / 2U;
  for (size_t start = 0; start < max_start_pair; start++) {
    for (uint8_t polarity = 0; polarity < 2; polarity++) {
      uint8_t hdr[2]{};
      if (!s1_decode_bytes_(raw, start, polarity != 0, hdr, 2))
        continue;
      const uint8_t l_field = hdr[0];
      const uint8_t c_field = hdr[1];
      const size_t frame_len = (size_t) l_field + 1U;
      if (frame_len < 12U || frame_len > 260U)
        continue;
      if (c_field != 0x44 && c_field != 0x46)
        continue;

      const size_t pairs = s1_raw_len_from_l_(l_field) * 4U;
      size_t invalid = 0, checked = 0;
      for (size_t p = 0; p < pairs; p++) {
        uint8_t v = 0;
        if (((start + p) * 2U + 1U) >> 3 >= raw.size())
          break;
        checked++;
        if (!s1_chip_pair_(raw, start + p, polarity != 0, v))
          invalid++;
      }
      // Keep the four cleanest, measured as invalid pairs per checked pair.
      const Hit h{start, polarity, l_field, c_field, (uint16_t) invalid, (uint16_t) checked};
      size_t slot = hit_count < 4 ? hit_count : 4;
      if (slot == 4) {
        size_t worst = 0;
        for (size_t i = 1; i < 4; i++)
          if (hits[i].invalid * h.checked > hits[worst].invalid * h.checked)
            worst = i;
        if (hits[worst].invalid * (uint32_t) h.checked <= (uint32_t) h.invalid * hits[worst].checked)
          continue;
        slot = worst;
      } else {
        hit_count++;
      }
      hits[slot] = h;
    }
  }

  if (hit_count == 0) {
    ESP_LOGI(TAG, "S1 frame start: no valid L+C at any of %u chip positions (both polarities), captured=%u B",
             (unsigned) max_start_pair, (unsigned) raw.size());
    return;
  }
  for (size_t i = 0; i < hit_count; i++) {
    ESP_LOGI(TAG,
             "S1 frame candidate %u/%u: chip=%u (raw byte %u) polarity=%u L=0x%02X C=0x%02X "
             "-> frame %uB, invalid %u/%u",
             (unsigned) (i + 1), (unsigned) hit_count, (unsigned) hits[i].chip,
             (unsigned) (hits[i].chip / 4U), (unsigned) hits[i].polarity, hits[i].l, hits[i].c,
             (unsigned) hits[i].l + 1U, (unsigned) hits[i].invalid, (unsigned) hits[i].checked);
    // Per candidate, not for the top one only: the ranking is by invalid pairs
    // per checked pair, and a coincidence over a short implied frame can still
    // outrank the real header. Its erasure map is what gives it away.
    s1_log_erasure_map_(raw, hits[i].chip, hits[i].polarity != 0, hits[i].l, (unsigned) (i + 1));
  }
}

// Decode the S1 L- and C-fields from the start of the captured stream and
// return the exact number of raw Manchester bytes a complete format-A frame
// occupies, DLL CRC bytes included. Zero means "cannot tell", which is what
// makes capture_rx_stream_() run on to its 512-byte cap.
static size_t s1_expected_raw_len_(const std::vector<uint8_t> &raw) {
  if (raw.size() < 4)
    return 0;

  for (uint8_t polarity = 0; polarity < 2; polarity++) {
    // L-field, raw bytes 0-1. Every pair must decode: this value cuts the
    // capture, so a substituted bit here yields a wrong length rather than a
    // recoverable one. No tolerance.
    uint8_t l_field = 0;
    bool l_ok = true;
    for (size_t bit = 0; bit < 8; bit++) {
      uint8_t v = 0;
      if (!s1_chip_pair_(raw, bit, polarity != 0, v)) {
        l_ok = false;
        break;
      }
      l_field = (uint8_t) ((l_field << 1U) | v);
    }
    if (!l_ok)
      continue;

    const size_t frame_len = (size_t) l_field + 1U;
    if (frame_len < 12U || frame_len > 260U)
      continue;

    // C-field, raw bytes 2-3. Up to two invalid pairs tolerated, and only the
    // bits that did decode are compared against 0x44 / 0x46.
    //
    // The C-field is here to stop the complemented polarity selecting a
    // plausible but wrong L-field - it contributes nothing to the length. So
    // demanding it decode perfectly buys nothing and costs a great deal:
    // measured 2026-08-01 at the sensitivity threshold, a single chip error in
    // raw byte 3 turned 0x65 into 0x6D, no length was derived, and the capture
    // ran to the 512-byte cap collecting 85 ms of post-frame noise. The frame
    // itself had one bad pair in 776.
    //
    // Two masked bits still leave six to match, so the complement (0xBB against
    // 0x44) is not going to slip through.
    uint8_t c_bits = 0, c_mask = 0;
    size_t c_invalid = 0;
    for (size_t bit = 0; bit < 8; bit++) {
      uint8_t v = 0;
      c_bits = (uint8_t) (c_bits << 1U);
      c_mask = (uint8_t) (c_mask << 1U);
      if (s1_chip_pair_(raw, 8U + bit, polarity != 0, v)) {
        c_bits = (uint8_t) (c_bits | v);
        c_mask = (uint8_t) (c_mask | 1U);
      } else {
        c_invalid++;
      }
    }
    if (c_invalid > 2)
      continue;
    if ((c_bits & c_mask) != (0x44U & c_mask) && (c_bits & c_mask) != (0x46U & c_mask))
      continue;

    const size_t blocks = (l_field < 26U) ? 2U : (size_t) ((l_field - 26U) / 16U + 3U);
    const size_t decoded_with_crc = frame_len + 2U * blocks;
    return decoded_with_crc * 2U;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// t1_expected_raw_len_: how many raw (3-of-6 coded) bytes this T-mode frame
// occupies, derived from its own L field. The T counterpart of
// s1_expected_raw_len_() above, and it exists for the same reason: with no
// length the stream capture does not know where the frame ends, so it runs on
// to its byte cap collecting post-frame noise.
//
// That was not a corner case in T mode, it was every single capture, because
// both of the loop's early exits are unreachable in this configuration:
//
//   * `end_irq` waits for RX_DONE or TIMEOUT. RX_DONE cannot fire - the loop
//     pushes REG_RXTX_PAYLOAD_LEN ahead of the write pointer on every poll,
//     which is exactly what AN1200.53 asks for - and RX is armed continuous,
//     so there is no TIMEOUT either.
//   * `silence` waits 30 ms without new bytes. In continuous RX the
//     demodulator keeps producing bits out of noise once the frame has ended,
//     so bytes keep arriving, last_change_ms keeps being refreshed, and the
//     silence never happens. (The S-mode branch does reach this exit, which is
//     why the note there reads the other way round - a different packet
//     configuration, and not a claim that holds for T mode.)
//
// So every capture ran to `buffer_cap`: 512 bytes, 125 ms of deafness, and a
// short frame buried under repeated reads of a 256-byte circular buffer. That
// is where `long_gfsk_packets: true` spent its 7.5 dB - not in sensitivity,
// which is why no register ever explained it. Confirmed by the field data: the
// exit reason recorded in the RSSI diagnostics was `buffer_cap` every time,
// and 512 copied bytes means 512 bytes really did arrive.
//
// Two differences from the S-mode helper, both making this one simpler:
//
//   * No polarity search. Manchester has two, so s1_expected_raw_len_() tries
//     both and needs the C field to reject the complement. 3-of-6 has no
//     polarity ambiguity, so the C field buys nothing here and is not read -
//     one less byte that has to survive at the sensitivity threshold.
//   * A decoded byte is exactly 1.5 raw bytes, so encoded_size() from the
//     3-of-6 decoder answers directly instead of the S-mode x2.
//
// Returns 0 when no length can be derived; the caller then keeps its cap.
// ---------------------------------------------------------------------------
static size_t t1_expected_raw_len_(const std::vector<uint8_t> &raw) {
  // Two raw bytes carry the two 6-bit symbols that make up the L field (with
  // four bits to spare). That is the entire input this needs.
  if (raw.size() < 2)
    return 0;

  const std::vector<uint8_t> head(raw.begin(), raw.begin() + 2);
  const auto decoded = decode3of6(head);
  if (!decoded.has_value() || decoded->empty())
    return 0;

  const uint8_t l_field = (*decoded)[0];
  const size_t frame_len = (size_t) l_field + 1U;
  // Same bounds as the S-mode helper: under 12 there is no room for a DLL
  // header, over 260 no wM-Bus frame exists.
  if (frame_len < 12U || frame_len > 260U)
    return 0;

  // Format A block layout, identical in both modes: block 1 carries 10 bytes,
  // every further block 16, each followed by a 2-byte CRC.
  const size_t blocks = (l_field < 26U) ? 2U : (size_t) ((l_field - 26U) / 16U + 3U);
  const size_t decoded_with_crc = frame_len + 2U * blocks;
  return encoded_size(decoded_with_crc);
}

// ---------------------------------------------------------------------------
// wait_while_busy_: poll BUSY pin until low (command accepted).
// Bail out after 200 ms to avoid hanging on a broken SPI bus.
// ---------------------------------------------------------------------------
void SX1262::wait_while_busy_() {
  if (this->busy_pin_ == nullptr)
    return;
  const uint32_t start = millis();
  while (this->busy_pin_->digital_read()) {
    if ((millis() - start) > 200) {
      ESP_LOGW(TAG, "BUSY stuck high / linia BUSY utknela w stanie wysokim (>200ms)");
      break;
    }
    delay(1);
  }
}

// ---------------------------------------------------------------------------
// cmd_write_: send command opcode + argument bytes. BUSY checked before and after.
// ---------------------------------------------------------------------------
void SX1262::cmd_write_(uint8_t cmd, std::initializer_list<uint8_t> args) {
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(cmd);
  for (auto b : args)
    this->delegate_->transfer(b);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}

// ---------------------------------------------------------------------------
// cmd_read_: send command + args, discard mandatory status byte, read out_len bytes.
// ---------------------------------------------------------------------------
void SX1262::cmd_read_(uint8_t cmd, std::initializer_list<uint8_t> args, uint8_t *out, size_t out_len) {
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(cmd);
  for (auto b : args)
    this->delegate_->transfer(b);
  (void) this->delegate_->transfer(0x00);  // status
  for (size_t i = 0; i < out_len; i++)
    out[i] = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}

// ---------------------------------------------------------------------------
// write_register_: write bytes to a 16-bit register address via WriteRegister command.
// ---------------------------------------------------------------------------
void SX1262::write_register_(uint16_t addr, std::initializer_list<uint8_t> data) {
  uint8_t msb, lsb;
  u16_to_be(addr, msb, lsb);

  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_WRITE_REGISTER);
  this->delegate_->transfer(msb);
  this->delegate_->transfer(lsb);
  for (auto b : data)
    this->delegate_->transfer(b);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}


// ---------------------------------------------------------------------------
// read_register8_: read one byte from a 16-bit register.
// Protocol requires one dummy byte after the address before data clocks out.
// ---------------------------------------------------------------------------
uint8_t SX1262::read_register8_(uint16_t addr) {
  uint8_t msb, lsb;
  u16_to_be(addr, msb, lsb);

  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_READ_REGISTER);
  this->delegate_->transfer(msb);
  this->delegate_->transfer(lsb);
  this->delegate_->transfer(0x00);  // dummy
  uint8_t v = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
  return v;
}

// TEST INSTRUMENTATION: capture the register neighbourhoods that contain the
// undocumented GFSK baseline fields and the documented RSSI/AGC calibration
// table. The POR snapshot is retained until log_reg_status(), because setup()
// runs early enough that its INFO output may not reach an attached log client.
void SX1262::capture_candidate_registers_(
    std::array<uint8_t, CANDIDATE_REGISTER_COUNT> &snapshot) {
  struct RegisterRange {
    uint16_t first;
    uint16_t last;
  };
  static constexpr RegisterRange ranges[] = {
      {0x06A0, 0x06DF},  // packet/GFSK area: includes 0x06AC and 0x06D1
      {0x0880, 0x08BF},  // RSSI/AGC area: includes 0x089B..0x089D, 0x08B8/0x08B9
      {0x08E0, 0x08FF},  // AGC gain table: includes 0x08F5..0x08FB
  };

  size_t index = 0;
  for (const auto &range : ranges) {
    for (uint16_t address = range.first; address <= range.last; address++) {
      snapshot[index++] = this->read_register8_(address);
    }
  }
}

// Print a captured snapshot as sixteen bytes per line. This routine is
// read-only; it does not touch the radio and therefore cannot perturb RX.
void SX1262::log_candidate_registers_(
    const char *stage, const std::array<uint8_t, CANDIDATE_REGISTER_COUNT> &snapshot) {
  static constexpr uint16_t range_starts[] = {0x06A0, 0x0880, 0x08E0};
  static constexpr uint8_t range_lengths[] = {64, 64, 32};
  size_t index = 0;

  ESP_LOGI(TAG, "SX1262 REGISTER DUMP BEGIN stage=%s", stage != nullptr ? stage : "?");
  for (size_t range = 0; range < 3; range++) {
    for (uint8_t range_offset = 0; range_offset < range_lengths[range]; range_offset += 16) {
      const uint16_t base = (uint16_t) (range_starts[range] + range_offset);
      char line[96];
      int used = snprintf(line, sizeof(line), "%04X:", (unsigned) base);
      for (uint8_t offset = 0; offset < 16; offset++) {
        const uint8_t value = snapshot[index++];
        if (used > 0 && (size_t) used < sizeof(line)) {
          used += snprintf(line + used, sizeof(line) - (size_t) used, " %02X", value);
        }
      }
      ESP_LOGI(TAG, "SX1262 REG [%s] %s", stage != nullptr ? stage : "?", line);
    }
  }
  ESP_LOGI(TAG, "SX1262 REGISTER DUMP END stage=%s", stage != nullptr ? stage : "?");
}



// ---------------------------------------------------------------------------
// get_irq_status_: read 16-bit IRQ status register, return raw flag word.
// ---------------------------------------------------------------------------
uint16_t SX1262::get_irq_status_() {
  uint8_t st[2]{};
  this->cmd_read_(CMD_GET_IRQ_STATUS, {}, st, sizeof(st));
  return (uint16_t(st[0]) << 8) | uint16_t(st[1]);
}


// ---------------------------------------------------------------------------
// read_buffer_: read out_len bytes from the 256-byte circular HW buffer.
// ReadBuffer handles wrap-around automatically, so a single call is always safe.
// ---------------------------------------------------------------------------
void SX1262::read_buffer_(uint8_t offset, uint8_t *out, size_t out_len) {
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_READ_BUFFER);
  this->delegate_->transfer(offset);
  (void) this->delegate_->transfer(0x00);  // status
  for (size_t i = 0; i < out_len; i++)
    out[i] = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}

// ---------------------------------------------------------------------------
// read_rssi_inst_dbm_: instantaneous RSSI of whatever is on the air *right now*.
//
// Only meaningful while the radio is in RX and the frame is still being
// transmitted. Called after the frame it measures the empty channel, i.e. the
// noise floor - see the comments in load_rx_buffer_() / capture_rx_stream_().
// ---------------------------------------------------------------------------
int8_t SX1262::read_rssi_inst_dbm_() {
  uint8_t rssi_raw = 0;
  this->cmd_read_(CMD_GET_RSSI_INST, {}, &rssi_raw, 1);
  // sx126x_rssi_dbm_ rejects raw 0 (never written) and the whole impossible
  // top of the scale, so no separate check is needed here.
  return sx126x_rssi_dbm_(rssi_raw);
}

// ---------------------------------------------------------------------------
// read_packet_status_rssi_: raw RssiSync / RssiAvg from GetPacketStatus.
//
// GFSK response layout (datasheet Rev 2.2, GetPacketStatus): RxStatus,
// RssiSync, RssiAvg. Raw values are returned unconverted so the caller can tell
// "0 = never latched" apart from a genuine reading, and so the diagnostic log
// can show what the radio actually reported.
//
//   RssiSync - latched when the sync word matched, i.e. at the real start of
//              the frame. This is the per-frame signal level we want.
//   RssiAvg  - averaged over the whole RX window. The window is a fixed 255
//              bytes (SetPacketParams above), so for a 134-byte WMBus frame
//              more than half of it is post-transmission noise, which drags the
//              average down towards the floor.
// ---------------------------------------------------------------------------
void SX1262::read_packet_status_rssi_(uint8_t &raw_sync, uint8_t &raw_avg) {
  uint8_t ps[3]{};
  this->cmd_read_(CMD_GET_PACKET_STATUS, {}, ps, sizeof(ps));
  raw_sync = ps[1];
  raw_avg = ps[2];
}

// ---------------------------------------------------------------------------
// record_rssi_diag_: note which source the frame's RSSI came from.
//
// This runs in the receiver task, whose log output never reaches the API log
// stream, so it must not report anything itself - it only parks a snapshot for
// Radio::loop() to print from the main task (see take_rssi_diag()). The DEBUG
// line below is therefore UART-only and exists for serial-console debugging.
//
// Each receive path is reported once. The two paths derive the level from
// different registers, so seeing both is what makes the report worth having;
// after that the radio goes quiet instead of narrating every frame.
// ---------------------------------------------------------------------------
void SX1262::record_rssi_diag_(RxPath path, uint8_t raw_sync, uint8_t raw_avg, int8_t inflight,
                               uint16_t trigger_irq, const char *exit_reason) {
  const char *source = (this->last_rssi_dbm_ == RSSI_NOT_MEASURED) ? "none"
                     : (inflight != RSSI_NOT_MEASURED && this->last_rssi_dbm_ == inflight) ? "inflight"
                     : (raw_sync != 0 && this->last_rssi_dbm_ == sx126x_rssi_dbm_(raw_sync)) ? "sync"
                     : "avg";
  const char *path_str = (path == RxPath::STREAM) ? "stream" : "fifo";

  ESP_LOGD(TAG, "RSSI %ddBm from %s (path=%s RssiSync=0x%02X RssiAvg=0x%02X inflight=%ddBm)",
           (int) this->last_rssi_dbm_, source, path_str, (unsigned) raw_sync, (unsigned) raw_avg,
           (int) inflight);

  const uint8_t slot = (uint8_t) path;

  // Verbose mode reports every capture instead of the first one per path.
  //
  // One snapshot per boot is the right volume when frames are arriving: it
  // answers "which register did this level come from" and then stops. It is
  // useless when nothing is being decoded, because a single `first[8]` cannot
  // separate a real frame captured out of alignment from the detector firing
  // on noise - that needs a series.
  //
  // Back-pressure instead of overwriting: if the previous snapshot has not been
  // drained by Radio::loop() yet, this one is dropped. The alternative is
  // writing into a slot while the main task copies out of it, which yields a
  // torn snapshot - fields from two different captures in one log line, which
  // is worse than a missing line precisely because it still looks like data.
  if (this->diag_verbose_) {
    if (this->rssi_diag_pending_[slot])
      return;
  } else {
    if (this->rssi_diag_reported_[slot])
      return;
  }
  // Claim the slot before filling it, so a second frame on this same path
  // cannot start another write into it.
  this->rssi_diag_reported_[slot] = true;

  this->rssi_diag_[slot].path = path_str;
  this->rssi_diag_[slot].source = source;
  this->rssi_diag_[slot].raw_sync = raw_sync;
  this->rssi_diag_[slot].raw_avg = raw_avg;
  this->rssi_diag_[slot].inflight = inflight;
  this->rssi_diag_[slot].result = this->last_rssi_dbm_;
  this->rssi_diag_[slot].trigger_irq = trigger_irq;
  this->rssi_diag_[slot].exit_reason = exit_reason;
  if (path == RxPath::STREAM) {
    this->rssi_diag_[slot].captured = (uint16_t) this->rx_buffer_.size();
    const size_t first_len =
        std::min<size_t>(this->rx_buffer_.size(), sizeof(this->rssi_diag_[slot].first_bytes));
    this->rssi_diag_[slot].first_len = (uint8_t) first_len;
    for (size_t i = 0; i < first_len; i++)
      this->rssi_diag_[slot].first_bytes[i] = this->rx_buffer_[i];
  }
  // Published last: the main task treats this flag as "snapshot is complete".
  this->rssi_diag_pending_[slot] = true;
}

bool SX1262::take_rssi_diag(RssiDiag &out) {
  for (uint8_t slot = 0; slot < 2; slot++) {
    if (!this->rssi_diag_pending_[slot])
      continue;
    out = this->rssi_diag_[slot];
    this->rssi_diag_pending_[slot] = false;
    return true;
  }
  return false;
}


// ---------------------------------------------------------------------------
// set_rf_frequency_: convert Hz to 25-bit PLL word and program it.
// Formula: rfFreq = (freq_hz * 2^25) / XTAL_FREQ
// ---------------------------------------------------------------------------
void SX1262::set_rf_frequency_(uint32_t freq_hz) {
  uint64_t rf = ((uint64_t) freq_hz << 25) / XTAL_FREQ;
  cmd_write_(CMD_SET_RF_FREQUENCY,
             {(uint8_t) ((rf >> 24) & 0xFF), (uint8_t) ((rf >> 16) & 0xFF), (uint8_t) ((rf >> 8) & 0xFF),
              (uint8_t) (rf & 0xFF)});
}

// ---------------------------------------------------------------------------
// set_sync_word_: program the 16-bit WMBus sync word into the register bank.
//   Byte 0 = 0x54 (WMBus constant), sync2 selects format:
//     0x3D = C-mode Format B (most common), 0xCD = C-mode Format A.
// ---------------------------------------------------------------------------
void SX1262::set_sync_word_(uint8_t sync2) {
  this->write_register_(REG_SYNC_WORD_0, {0x54, sync2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

void SX1262::set_s1_sync_word_() {
  // S-mode sync is 18 bits (0x7696) preceded here by 3 x "01" preamble bits,
  // commonly programmed as 0x54 0x76 0x96 in packet radios.
  this->write_register_(REG_SYNC_WORD_0, {0x54, 0x76, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00});
}

bool SX1262::has_rx_done_() {
  uint8_t irq[2]{};
  this->cmd_read_(CMD_GET_IRQ_STATUS, {}, irq, sizeof(irq));
  const uint16_t flags = ((uint16_t) irq[0] << 8) | irq[1];
  return (flags & IRQ_RX_DONE) != 0;
}

bool SX1262::long_stream_active_() const {
  return this->long_gfsk_packets_ && this->long_stream_hold_until_ms_ != 0 &&
         (int32_t) (this->long_stream_hold_until_ms_ - millis()) > 0;
}

void SX1262::configure_irq_params_() {
  const bool stream_on_sync = this->long_stream_active_() || (this->listen_mode_ == LISTEN_MODE_S1);
  const uint16_t mask = stream_on_sync ? (IRQ_SYNC_WORD_VALID | IRQ_RX_DONE | IRQ_CRC_ERROR | IRQ_TIMEOUT)
                                       : (IRQ_RX_DONE | IRQ_CRC_ERROR | IRQ_TIMEOUT);
  const uint8_t mask_msb = (uint8_t) ((mask >> 8) & 0xFF);
  const uint8_t mask_lsb = (uint8_t) (mask & 0xFF);

  this->cmd_write_(CMD_SET_DIO_IRQ_PARAMS,
                   {mask_msb, mask_lsb,  // IRQ mask
                    mask_msb, mask_lsb,  // DIO1 mask
                    0x00, 0x00,          // DIO2 mask
                    0x00, 0x00});        // DIO3 mask
}

// ---------------------------------------------------------------------------
// load_rx_buffer_: normal (non-long-packet) receive path.
// After RX_DONE, reads payload length + start pointer, loads rx_buffer_, caches RSSI.
// ---------------------------------------------------------------------------
bool SX1262::load_rx_buffer_() {
  if (!this->has_rx_done_())
    return false;

  uint8_t st[2]{};
  this->cmd_read_(CMD_GET_RX_BUFFER_STATUS, {}, st, sizeof(st));
  const uint8_t payload_len = st[0];
  const uint8_t start_ptr = st[1];

  if (payload_len == 0) {
    this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});
    return false;
  }

  this->rx_buffer_.assign(payload_len, 0);

  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_READ_BUFFER);
  this->delegate_->transfer(start_ptr);
  (void) this->delegate_->transfer(0x00);
  for (size_t i = 0; i < this->rx_buffer_.size(); i++)
    this->rx_buffer_[i] = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();

  // ---------------------------------------------------------------------------
  // Packet RSSI.
  //
  // This path only runs after RX_DONE, so the transmitter is already silent:
  // an instantaneous GetRssiInst here measures the empty channel, not the
  // frame. That fallback is what made every meter report the same near-floor
  // level regardless of distance, so it is deliberately gone.
  //
  // RssiSync is latched at sync-word detection and is the only value that
  // describes this frame; RssiAvg is only a fallback because the fixed 255-byte
  // RX window mixes a long noise tail into the average. If the radio latched
  // neither, report "not measured" rather than inventing a number.
  // ---------------------------------------------------------------------------
  {
    uint8_t raw_sync = 0, raw_avg = 0;
    this->read_packet_status_rssi_(raw_sync, raw_avg);
    // The converter returns the sentinel for anything that cannot be a real
    // level, so each candidate is asked in turn and the first usable one wins.
    // Testing the raw bytes for != 0 instead would let an implausible sync
    // value shadow a perfectly good average.
    const int8_t from_sync = sx126x_rssi_dbm_(raw_sync);
    const int8_t from_avg = sx126x_rssi_dbm_(raw_avg);
    if (from_sync != RSSI_NOT_MEASURED) {
      this->last_rssi_dbm_ = from_sync;
    } else if (from_avg != RSSI_NOT_MEASURED) {
      this->last_rssi_dbm_ = from_avg;
    } else {
      this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
    }
    this->record_rssi_diag_(RxPath::FIFO, raw_sync, raw_avg, RSSI_NOT_MEASURED);
  }

  this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});

  this->rx_idx_ = 0;
  this->rx_len_ = this->rx_buffer_.size();
  this->rx_loaded_ = true;
  return true;
}


// ---------------------------------------------------------------------------
// capture_rx_stream_: long-packet receive path, implements Semtech AN1200.53.
//
// The SX126x has a 256-byte circular HW buffer. In normal mode the radio stops
// at REG_RXTX_PAYLOAD_LEN, limiting a burst to 255 bytes. WMBus T1 with 3-of-6
// encoding can exceed this limit.
//
// AN1200.53 trick:
//   1. Set REG_RXTX_PAYLOAD_LEN = 0xFF so the radio never auto-stops.
//   2. Poll REG_RX_ADDR_PTR to see how many new bytes arrived.
//   3. Drain those bytes into rx_buffer_.
//   4. Write (current_ptr - 1) back to REG_RXTX_PAYLOAD_LEN to keep the radio alive.
//   5. Repeat until IRQ_RX_DONE / IRQ_TIMEOUT fires and data flow stops.
//
// avail = (uint8_t)(cur - state_index): intentional uint8_t wrap handles
// the 0xFF->0x00 circular pointer rollover (e.g. cur=0x0A, state=0xFA -> avail=16).
//
// The manual first/second read split below is a conservative defensive measure;
// ReadBuffer wraps automatically in HW so both produce identical results.
// ---------------------------------------------------------------------------
bool SX1262::capture_rx_stream_(uint16_t trigger_irq) {
  // Semtech AN1200.53: stream from the 256-byte internal buffer while RX is still running.
  // We rely on IRQ_RX_DONE / IRQ_TIMEOUT to decide when the burst is complete (not a tiny "silence" window),
  // otherwise we may cut valid frames (WMBus T1 airtime is several ms).
  this->rx_buffer_.clear();
  this->rx_buffer_.reserve(512);

  // Adaptive long-T1 enters this routine after the trigger, so preserve its
  // established late write. S1 follows AN1200.53 and writes 0xFF before SetRx
  // in restart_rx(); rewriting it after SyncWordValid would be too late.
  if (this->listen_mode_ != LISTEN_MODE_S1)
    this->write_register_(REG_RXTX_PAYLOAD_LEN, {0xFF});

  const uint32_t start_ms = millis();
  uint32_t last_change_ms = start_ms;

  size_t copied = 0;
  uint8_t state_index = 0;  // last read index (wraps 0..255)
  size_t s1_expected_raw_len = 0;
  size_t t1_expected_raw_len = 0;

  // Instrumentation for the capture itself, not for its contents.
  //
  // At 32.768 kchip/s one raw byte is 8 chips = 244 us, so 512 bytes cannot
  // physically arrive in less than 125 ms. If a capture reaches the cap faster
  // than that, the bytes are not coming off the air as they are read - the
  // buffer is being re-read, and the frame may well be in there while we grind
  // it into duplicates. That distinction decides whether buffer_cap is a
  // symptom or the cause, and nothing else in the log answers it.
  // Byte budget for this capture. Stays at 512 while a length is still
  // possible; drops to 256 the moment the S1 header is known to be
  // undecodable. See the S1 block in the loop for why that moment is knowable.
  size_t capture_cap = 512;

  uint32_t poll_iterations = 0;
  uint32_t reads_with_data = 0;
  uint8_t max_avail = 0;

  // Capture until RX_DONE/TIMEOUT (latched IRQ), then allow a short drain window.
  bool seen_end_irq = false;
  const char *exit_reason = "unknown";

  // Peak-hold of the instantaneous RSSI sampled while bytes were arriving.
  // See the RSSI block after the loop for why this is the primary source here.
  int8_t rssi_inflight = RSSI_NOT_MEASURED;

  while (true) {
    const uint32_t now = millis();
    poll_iterations++;

    // Safety: don't hang forever
    if ((now - start_ms) > 250) {
      ESP_LOGD(TAG, "Long RX capture timeout, copied=%u", (unsigned) copied);
      exit_reason = "safety_timeout";
      break;
    }
    // Hard cap (covers max WMBus T1 raw size comfortably)
    if (copied >= capture_cap) {
      ESP_LOGD(TAG, "Long RX capture capped at %u bytes", (unsigned) capture_cap);
      exit_reason = "buffer_cap";
      break;
    }

    const uint8_t cur = this->read_register8_(REG_RX_ADDR_PTR);
    uint8_t avail = (uint8_t) (cur - state_index);  // uint8 wrap by design

    // Cap to remaining space
    const size_t room = capture_cap - copied;
    if (avail > room) {
      avail = (uint8_t) room;
    }

    // Compute a "virtual" current_index (may be < cur if capped)
    const uint8_t current_index = (uint8_t) (state_index + avail);

    // Keep receiver alive: keep RxTxPldLen always behind the current write pointer.
    this->write_register_(REG_RXTX_PAYLOAD_LEN, {(uint8_t) (current_index - 1)});

    if (avail != 0) {
      reads_with_data++;
      if (avail > max_avail)
        max_avail = avail;
      // Sample RSSI before draining: bytes are arriving right now, so the
      // transmitter is still on air and this is the frame's own signal level.
      // Draining 200+ bytes over SPI takes long enough that a sample taken
      // afterwards can already fall past the end of the transmission.
      const int8_t inst = this->read_rssi_inst_dbm_();
      if (inst != RSSI_NOT_MEASURED && inst > rssi_inflight)
        rssi_inflight = inst;

      uint8_t tmp[256];
      const uint8_t off = state_index;
      const uint8_t first = (uint8_t) ((off + avail <= 256) ? avail : (256 - off));
      this->read_buffer_(off, tmp, first);
      if (avail > first) {
        this->read_buffer_(0, tmp + first, (size_t) (avail - first));
      }
      this->rx_buffer_.insert(this->rx_buffer_.end(), tmp, tmp + avail);
      copied += avail;
      state_index = current_index;
      last_change_ms = now;

      if (this->listen_mode_ == LISTEN_MODE_S1) {
        if (s1_expected_raw_len == 0) {
          s1_expected_raw_len = s1_expected_raw_len_(this->rx_buffer_);
          // s1_expected_raw_len_() only ever reads raw bytes 0..3. Once it has
          // seen them and produced nothing, it will keep producing nothing for
          // the rest of this capture - those bytes do not change. The frame is
          // already lost at that point and the only question left is how long
          // to stay deaf gathering evidence about it.
          //
          // 512 bytes is 127 ms of that, measured. Half is enough to see what
          // the stream looked like, and gives the receiver back 63 ms sooner.
          // The full budget is kept whenever a length IS derivable, because a
          // legitimate long S-mode frame needs it: L=150 works out to about
          // 340 raw bytes, which a fixed 256-byte cap would truncate.
          if (s1_expected_raw_len == 0 && this->rx_buffer_.size() >= 4)
            capture_cap = 256;
        }
        if (s1_expected_raw_len != 0 && copied >= s1_expected_raw_len) {
          this->rx_buffer_.resize(s1_expected_raw_len);
          copied = s1_expected_raw_len;
          exit_reason = "s1_length";
          break;
        }
      } else if (this->listen_mode_ == LISTEN_MODE_T1) {
        if (t1_expected_raw_len == 0) {
          t1_expected_raw_len = t1_expected_raw_len_(this->rx_buffer_);
          // Same argument as the S-mode branch: the helper only ever reads raw
          // bytes 0..1, so once those have arrived and yielded nothing they
          // will keep yielding nothing. Without an L field nothing downstream
          // can find the block boundaries either, so the frame is already lost.
          //
          // Where this differs from S-mode: stop NOW rather than gathering a
          // shorter window. Every further byte is time spent deaf over a frame
          // that will be discarded anyway, and this is the one path that fires
          // more often the weaker the signal - so continuing costs exactly the
          // frames that were hardest to catch in the first place. That feedback
          // is a candidate for the ~7 dB this mode still loses in weak signal
          // (measured 2026-09-02/03, off -> on -> off on one board).
          //
          // Verbose keeps the old 256-byte window: telling a genuine weak frame
          // from a noise trigger needs to see what the stream looked like, and
          // that is worth 63 ms when somebody is deliberately watching.
          if (t1_expected_raw_len == 0 && this->rx_buffer_.size() >= 2) {
            if (this->diag_verbose_) {
              capture_cap = 256;
            } else {
              exit_reason = "t1_no_length";
              break;
            }
          }
        }
        if (t1_expected_raw_len != 0 && copied >= t1_expected_raw_len) {
          // Trim the noise the radio wrote after the frame ended. Everything
          // past this point is post-frame demodulation, never frame content.
          this->rx_buffer_.resize(t1_expected_raw_len);
          copied = t1_expected_raw_len;
          exit_reason = "t1_length";
          break;
        }
      }
    } else {
      // No new bytes right now.
      const uint16_t irq = this->get_irq_status_();
      if (irq & (IRQ_RX_DONE | IRQ_TIMEOUT)) {
        seen_end_irq = true;
      }

      // After we saw end IRQ, wait a short "drain" window to pull remaining bytes, then stop.
      if (seen_end_irq && (now - last_change_ms) > 15) {
        exit_reason = "end_irq";
        break;
      }

      // S1 / short packets: IRQ_RX_DONE never fires in FIXED_LEN + payload_len=0xFF
      // when the packet is shorter than 255 bytes (radio keeps waiting for more bytes
      // that never arrive). Without this exit the loop spins for the full 250ms safety
      // timeout, creating a window where a second meter's bytes get mixed into our
      // rx_buffer_ and corrupt both decodes. 30ms silence at 32.768 kcps (Manchester)
      // is unambiguously end-of-packet.
      if (copied > 0 && (now - last_change_ms) > 30) {
        exit_reason = "silence";
        break;
      }

      delay(1);
    }
  }

  // ---------------------------------------------------------------------------
  // Packet RSSI for the streamed path. Must be read BEFORE stopping RX
  // (standby), otherwise GetPacketStatus returns zeros.
  //
  // Here the packet engine is deliberately never allowed to finish - RxTxPldLen
  // is pushed forward on every iteration - so GetPacketStatus may still hold
  // values latched by an *older* frame, or nothing at all. The in-flight peak
  // sampled while bytes were arriving is therefore the primary source; packet
  // status only covers the case where the frame was already fully buffered
  // before we got here (entry on RX_DONE rather than SYNC_WORD_VALID).
  //
  // Sampling GetRssiInst at this point instead - which is what the old code did
  // whenever RssiAvg read back as 0 - measures the channel several tens of
  // milliseconds after the transmission ended, i.e. the noise floor. That is
  // why every meter reported the same level.
  // ---------------------------------------------------------------------------
  uint8_t raw_sync = 0, raw_avg = 0;
  this->read_packet_status_rssi_(raw_sync, raw_avg);
  const int8_t from_sync = sx126x_rssi_dbm_(raw_sync);
  const int8_t from_avg = sx126x_rssi_dbm_(raw_avg);
  if (rssi_inflight != RSSI_NOT_MEASURED) {
    this->last_rssi_dbm_ = rssi_inflight;
  } else if (from_sync != RSSI_NOT_MEASURED) {
    this->last_rssi_dbm_ = from_sync;
  } else if (from_avg != RSSI_NOT_MEASURED) {
    this->last_rssi_dbm_ = from_avg;
  } else {
    this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
  }
  this->record_rssi_diag_(RxPath::STREAM, raw_sync, raw_avg, rssi_inflight,
                          trigger_irq, exit_reason);

  // Where does the frame actually start in what we just captured? Verbose only:
  // it is a search over up to 512 chip offsets and belongs nowhere near a node
  // that is receiving normally.
  if (this->diag_verbose_ && this->listen_mode_ == LISTEN_MODE_S1 && !this->rx_buffer_.empty()) {
    // 8 chips per raw byte at 32768 chip/s = 244.14 us per byte, so
    // ms = bytes * 8 * 1000 / 32768, which reduces to bytes * 2000 / 8192.
    const uint32_t air_ms = (uint32_t) ((this->rx_buffer_.size() * 2000UL) / 8192UL);
    const uint32_t took_ms = millis() - start_ms;
    ESP_LOGI(TAG,
             "S1 capture timing: %u B in %ums (air time for %u B is %ums) exit=%s "
             "polls=%u with_data=%u max_avail=%u",
             (unsigned) this->rx_buffer_.size(), (unsigned) took_ms,
             (unsigned) this->rx_buffer_.size(), (unsigned) air_ms, exit_reason,
             (unsigned) poll_iterations, (unsigned) reads_with_data, (unsigned) max_avail);
    this->log_s1_frame_start_(this->rx_buffer_);
  }

  // Stop RX and clear IRQs.
  this->cmd_write_(CMD_SET_STANDBY, {STANDBY_RC});

  // The device-error snapshot used to live here, gated on the first captured
  // frame. It has moved to setup(), where "on boot" is actually true: gating it
  // on a frame meant a node that receives nothing never cleared the power-on
  // XOSC_START_ERR, and that node is the only one whose error register anybody
  // reads. Removing it from the receive path also takes a 7 ms blocking delay
  // off the first capture.
  this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});

  if (this->rx_buffer_.empty())
    return false;

  this->rx_idx_ = 0;
  this->rx_len_ = this->rx_buffer_.size();
  this->rx_loaded_ = true;

  // Renew the adaptive hold if the captured frame is still long (>= 250 bytes).
  // Without this the hold expires between consecutive transmissions of the same
  // long-packet meter, causing the next frame to fall back to the FIFO path
  // (truncated) and re-trigger from scratch.
  if (this->long_gfsk_packets_ && this->rx_len_ >= 250) {
    this->long_stream_hold_until_ms_ = millis() + 45000UL;
    ESP_LOGD(TAG, "Long RX captured %u bytes (hold renewed for 45 s)", (unsigned) this->rx_len_);
  } else {
    ESP_LOGD(TAG, "Long RX captured %u bytes", (unsigned) this->rx_len_);
  }
  return true;
}



// ---------------------------------------------------------------------------
// setup: called once by ESPHome at boot.
// Configures FEM pins (if defined), resets the SX1262, and programs all
// static RF / modulation / packet parameters.
// ---------------------------------------------------------------------------
void SX1262::setup() {
  this->irq_edge_ = gpio::INTERRUPT_RISING_EDGE;
  this->common_setup();
  ESP_LOGV(TAG, "Setup");
  {
    const char *lm = (this->listen_mode_ == LISTEN_MODE_T1) ? "T1 only"
                   : (this->listen_mode_ == LISTEN_MODE_C1) ? "C1 only"
                   : (this->listen_mode_ == LISTEN_MODE_S1) ? "S1 only"
                   : "T1+C1 (both, 3:1 bias)";
    ESP_LOGI(TAG, "Listen mode / tryb nasluchu: %s", lm);
  }

  // MUST be before any SPI transfers
  this->spi_setup();

  // Optional FEM pins (if used instead of YAML switch/output).
  //
  // Power-on sequence follows the FEM datasheet, not source order: FEM supply
  // (POWER/VFEM) MUST be asserted before any control line. GC1109 (Heltec V4.2)
  // Table 3 note 2: "VBAT must be prior to CSD/CPS/CTX for the power on
  // sequence", note 1: "CSD/CPS/CTX voltage level must not exceed VBAT".
  // KCT8103L (V4-R8) has the same CSD/CPS/CTX logic. Heltec's own driver does
  // POWER -> CSD -> CTX with a 1 ms settle; we mirror that here.
  //
  //   fem_ctrl -> POWER/VFEM (HIGH, enable supply)
  //   fem_en   -> CSD        (HIGH, RX select)
  //   fem_pa   -> CTX        (LOW,  TX disabled -> RX path)
  if (this->fem_ctrl_pin_ != nullptr) {
    this->fem_ctrl_pin_->setup();
    this->fem_ctrl_pin_->digital_write(true);
    delay(1);  // let VFEM settle before driving control lines
  }
  if (this->fem_en_pin_ != nullptr) {
    this->fem_en_pin_->setup();
    this->fem_en_pin_->digital_write(true);
  }
  if (this->fem_pa_pin_ != nullptr) {
    this->fem_pa_pin_->setup();
    this->fem_pa_pin_->digital_write(false);
  }

  // External gate of the module's internal RF switch.
  //
  // Some SX1262 modules do not connect the antenna unconditionally. The
  // Seeed Wio-SX1262 brings the gate out on module pin 1 (RF_SW, "External IO
  // control internal gate RF switch") and expects the host to hold it high;
  // on the XIAO ESP32S3 kit that lands on GPIO38. This is separate from
  // dio2_rf_switch: per the module datasheet the TX/RX *direction* is chosen
  // by the SX1262's own DIO2 (high = TX, low = RX), while RF_SW decides
  // whether the switch conducts at all.
  //
  // Leaving it unconfigured is not a soft degradation. The pin idles as a
  // high-impedance input after reset, the antenna path never closes, and the
  // receiver hears only leakage - measured at roughly 50 dB below an
  // equivalent board, which looks exactly like a broken antenna.
  if (this->rf_sw_pin_ != nullptr) {
    this->rf_sw_pin_->setup();
    this->rf_sw_pin_->digital_write(true);
    delay(1);  // let the switch settle before the radio is brought up
  }
  ESP_LOGI(TAG, "RF switch gate / bramka przelacznika RF: %s",
           (this->rf_sw_pin_ != nullptr) ? "driven high (rf_sw_pin) / sterowana"
                                         : "not configured / nieskonfigurowana");

  this->reset();
  delay(10);

  // TEST INSTRUMENTATION: retain the POR baseline before this driver changes
  // RX gain, applies the RadioLib GFSK fix, or runs calibration/configuration.
  this->capture_candidate_registers_(this->reset_register_snapshot_);
  this->reset_register_snapshot_valid_ = true;

  // Apply RX gain (datasheet values)
  const uint8_t gain =
      (this->rx_gain_ == SX1262RxGain::POWER_SAVING) ? RX_GAIN_POWER_SAVING : RX_GAIN_BOOSTED;
  this->write_register_(REG_RX_GAIN, {gain});
  ESP_LOGI(TAG, "RX gain / wzmocnienie RX: %s", (this->rx_gain_ == SX1262RxGain::POWER_SAVING) ? "POWER_SAVING" : "BOOSTED");

  // GFSK baseline fix, ported from RadioLib's fixGFSK() "reset" branch (see
  // REG_GFSK_FIX_1/3/4 and REG_RSSI_AVG_WINDOW comments above). This driver
  // has never written any of these four registers, so every SX1262 board in
  // this project has been running on whatever value survived chip reset -
  // never verified against what Semtech's own reference code sets. Each
  // write below is read-modify-write over the stated bit field only; every
  // other bit in the byte is left exactly as the chip had it.
  {
    uint8_t v;
    v = this->read_register8_(REG_GFSK_FIX_1);
    v = (uint8_t) ((v & ~0x18) | 0x08);  // bits[4:3] = 01
    this->write_register_(REG_GFSK_FIX_1, {v});

    v = this->read_register8_(REG_RSSI_AVG_WINDOW);
    v = (uint8_t) (v & ~0x1C);  // bits[4:2] = 000
    this->write_register_(REG_RSSI_AVG_WINDOW, {v});

    v = this->read_register8_(REG_GFSK_FIX_3);
    v = (uint8_t) (v | 0x10);  // bit[4] = 1
    this->write_register_(REG_GFSK_FIX_3, {v});

    v = this->read_register8_(REG_GFSK_FIX_4);
    v = (uint8_t) (v & ~0x70);  // bits[6:4] = 000
    this->write_register_(REG_GFSK_FIX_4, {v});

    ESP_LOGI(TAG, "GFSK baseline fix applied (RadioLib fixGFSK reset branch, undocumented in datasheet)");
  }

  this->cmd_write_(CMD_SET_STANDBY, {STANDBY_RC});

  // DIO2 RF switch
  this->cmd_write_(CMD_SET_DIO2_AS_RF_SWITCH_CTRL, {uint8_t(this->dio2_rf_switch_ ? 0x01 : 0x00)});

  // TCXO only if enabled.
  // SetDIO3AsTcxoCtrl(tcxoVoltage, timeout[23:0]) per SX1261/2 datasheet:
  // tcxo_voltage_ (default 3.0V, see SX1262TcxoVoltage) selects DIO3's
  // regulated output, and the 24-bit timeout counts in 15.625 us steps, so
  // 0x000040 = 64 steps = 1 ms of TCXO start-up time before the chip
  // considers the reference stable.
  //
  // The recalibration below is not optional and not tuning. At power-on the
  // chip calibrates itself against whatever reference it has, which at that
  // point is the internal RC oscillator - the TCXO is still unpowered, because
  // it is DIO3 that powers it and DIO3 has just been told so on the line above.
  // Every block calibrated before this point therefore sits on a reference that
  // no longer exists. The datasheet states the requirement directly: after
  // SetDIO3AsTcxoCtrl the calibration must be relaunched.
  //
  // Skipping it does not fail loudly. The radio starts, arms RX, and receives a
  // transmitter on the same desk perfectly well; what it loses is the last few
  // dB, which is exactly the band the real meters live in. Nothing in the log
  // distinguishes that from a bad antenna, which is why the device-error
  // readback further down was added together with this call.
  if (this->has_tcxo_) {
    this->cmd_write_(CMD_SET_DIO3_AS_TCXO_CTRL, {(uint8_t) this->tcxo_voltage_, 0x00, 0x00, 0x40});
    delay(5);
    this->cmd_write_(CMD_CALIBRATE, {CALIBRATE_ALL});
    // Calibrate holds BUSY for up to 3.5 ms per the datasheet. cmd_write_ already
    // waits on the pin; the delay covers boards that leave BUSY unconnected.
    delay(10);
  }

  // CalibrateImage(freq1, freq2) per SX1261/2 datasheet: the two bytes select the
  // band the image rejection is calibrated for. 0xD7/0xDB is the datasheet entry
  // for 863-870 MHz, i.e. the wM-Bus EU band this bridge listens on. Changing the
  // configured frequency outside that band would need the matching pair.
  this->cmd_write_(CMD_CALIBRATE_IMAGE, {0xD7, 0xDB});

  // ---------------------------------------------------------------------------
  // Clear the latched device errors, here rather than on the first frame.
  //
  // XOSC_START_ERR is set during the power-on sequence as a matter of course on
  // a TCXO board: the chip tries to start its crystal oscillator before DIO3
  // has been told to power the TCXO, because DIO3 is configured after reset.
  // The datasheet expects the flag to be cleared once the reference is properly
  // set up. Until that happens the register reports a startup that failed
  // minutes ago and has since been fixed.
  //
  // This used to live in capture_rx_stream_(), gated on the first captured
  // frame. On a node receiving normally the first frame arrives within seconds,
  // the flag clears, and nobody ever saw it. On a node receiving nothing it
  // never ran at all - so `clear_device_errors_on_boot` did nothing precisely
  // in the case where the error register is the only thing left to read.
  // Observed 2026-08-01: two SX1262 nodes reporting XOSC_START_ERR for minutes
  // while sitting in RX with a correct configuration.
  //
  // The re-read at the end of setup() is what makes this worth doing: an error
  // that clears was a power-on artefact, an error that comes back after a clean
  // clear is a reference that genuinely is not starting.
  // ---------------------------------------------------------------------------
  if (this->clear_device_errors_on_boot_) {
    uint8_t de[2]{};
    this->cmd_read_(CMD_GET_DEVICE_ERRORS, {}, de, sizeof(de));
    this->boot_dev_err_before_ = u16be_(de);

    // ClearDeviceErrors can be ignored outside standby; setup() has not left
    // STANDBY_RC since it was entered above, but say so explicitly rather than
    // depend on the reader tracking the chip state across thirty lines.
    this->cmd_write_(CMD_SET_STANDBY, {STANDBY_RC});
    delay(5);
    this->cmd_write_(CMD_CLEAR_DEVICE_ERRORS, {0x00, 0x00});
    delay(2);

    this->cmd_read_(CMD_GET_DEVICE_ERRORS, {}, de, sizeof(de));
    this->boot_dev_err_after_ = u16be_(de);
    this->boot_dev_err_valid_ = true;

    ESP_LOGI(TAG, "Device errors cleared on boot / bledy ukladu wyczyszczone przy starcie: 0x%04X -> 0x%04X",
             this->boot_dev_err_before_, this->boot_dev_err_after_);
  }

  this->cmd_write_(CMD_SET_PACKET_TYPE, {PACKET_TYPE_GFSK});
  this->set_rf_frequency_(this->configured_frequency_hz_);

  this->cmd_write_(CMD_SET_BUFFER_BASE_ADDRESS, {0x00, 0x00});

  // Modulation params. T1/C1 use 100 kbps; S1 uses 32.768 kcps Manchester.
  const uint32_t bitrate = (this->listen_mode_ == LISTEN_MODE_S1) ? 32768UL : 100000UL;
  const uint32_t br = (XTAL_FREQ * 32UL) / bitrate;

  const uint32_t freq_dev = (this->listen_mode_ == LISTEN_MODE_C1) ? 45000UL : 50000UL;
  const uint32_t fdev = ((uint64_t) freq_dev << 25) / XTAL_FREQ;
  // S1 stays at 234.3 kHz. Measured, not argued.
  //
  // Three-point sweep, 2026-08-01. Each figure is the longest run of valid
  // Manchester pairs in an SX1262 capture, taken on a transmission an SX1276
  // decoded in the same second, out of the 680 pairs an 85-byte telegram needs.
  // Random data gives about 11.
  //
  //   312.0 kHz  ->  30 pairs
  //   234.3 kHz  -> 191 pairs   <- optimum
  //   156.2 kHz  ->  47 pairs
  //
  // Both directions cost a factor of four to six, so this is a genuine peak and
  // not a monotonic trend anyone can keep chasing. Widening admits noise;
  // narrowing starts cutting the signal, whose occupied spectrum is wider than
  // either Carson (133 kHz) or Semtech's sizing rule (143 kHz) predicts for a
  // Manchester-coded BT=0.5 chip stream.
  //
  // Worth stating plainly: at the optimum the capture still holds 191 of the
  // 680 pairs a frame needs. Bandwidth is not what stops the SX1262 decoding
  // S-mode - it is worth about a factor of four and no more. Do not re-run this
  // sweep; the numbers are here.
  // T1 is the one mode the sweep above never covered - it was run on S-mode.
  // 312.0 kHz here is inherited, not measured, and it is 25% wider than the
  // 250 kHz the SX1276 uses for T1, so it admits ~1.2 dB of noise the signal
  // does not need. `sx1262_rx_bandwidth` exists to settle that on hardware.
  // Scope, exactly: `listen_mode: c1` and `listen_mode: s1` ignore this and
  // keep their measured 234.3 kHz. `listen_mode: both` does NOT - it shares one
  // receiver bandwidth with T1 and follows this setting, which is also what it
  // did before the option existed (it took the 312 kHz T1 branch).
  uint8_t rx_bw = GFSK_RX_BW_234_3;
  if (this->listen_mode_ != LISTEN_MODE_C1 && this->listen_mode_ != LISTEN_MODE_S1) {
    switch (this->t1_rx_bandwidth_) {
      case T1_BW_234: rx_bw = GFSK_RX_BW_234_3; break;
      case T1_BW_156: rx_bw = GFSK_RX_BW_156_2; break;
      case T1_BW_312:
      default:        rx_bw = GFSK_RX_BW_312_0; break;
    }
  }

  {
    char buf[96];
    snprintf(buf, sizeof(buf), "freq=%.3fMHz bitrate=%lukbps fdev=%lukHz BT=0.5 RxBW=%s%s",
             this->configured_frequency_hz_ / 1000000.0f,
             (unsigned long) (bitrate / 1000UL),
             (unsigned long) (freq_dev / 1000UL),
             (rx_bw == GFSK_RX_BW_156_2) ? "156kHz" : (rx_bw == GFSK_RX_BW_234_3) ? "234kHz" : "312kHz",
             (this->listen_mode_ == LISTEN_MODE_S1) ? " Manchester/S-mode" : "");
    this->rf_params_str_ = buf;
  }

  this->cmd_write_(CMD_SET_MODULATION_PARAMS,
                   {(uint8_t) ((br >> 16) & 0xFF), (uint8_t) ((br >> 8) & 0xFF), (uint8_t) (br & 0xFF),
                    GFSK_PULSE_SHAPE_BT_0_5, rx_bw, (uint8_t) ((fdev >> 16) & 0xFF),
                    (uint8_t) ((fdev >> 8) & 0xFF), (uint8_t) (fdev & 0xFF)});

  // Packet params
  const uint16_t preamble_bits = 64;
  const uint8_t preamble_msb = (uint8_t) ((preamble_bits >> 8) & 0xFF);
  const uint8_t preamble_lsb = (uint8_t) (preamble_bits & 0xFF);

  // Always FIX_LEN for WMBus.
  const uint8_t pkt_len_mode = GFSK_PACKET_FIX_LEN;
  this->cmd_write_(CMD_SET_PACKET_PARAMS,
                   {preamble_msb, preamble_lsb, (uint8_t) this->preamble_detector_,
                    S1_SYNC_WORD_BITS_OR_DEFAULT(this->listen_mode_),
                    GFSK_ADDRESS_FILT_OFF, pkt_len_mode,
                    0xFF,  // max payload
                    GFSK_CRC_OFF, GFSK_WHITENING_OFF});

  // IRQ routing -> DIO1.
  // Keep the fast RX_DONE-only path by default. When adaptive long-stream hold
  // is active, restart_rx() will reconfigure this to include SYNC_WORD_VALID.
  this->configure_irq_params_();

  // Device errors after a complete init.
  //
  // A snapshot already existed, but it lives in capture_rx_stream_() and fires
  // on the first captured frame - i.e. never, in the one case worth diagnosing,
  // where nothing is being received. Reading the register here costs one SPI
  // transfer at boot and is the only evidence that the calibration above
  // actually took: XOSC_START_ERR means the TCXO never started, PLL_CALIB_ERR
  // means the recalibration itself failed. Both are silent otherwise.
  {
    uint8_t de[2]{};
    this->cmd_read_(CMD_GET_DEVICE_ERRORS, {}, de, sizeof(de));
    const uint16_t errors = u16be_(de);
    if (errors & (DEV_ERR_XOSC_START | DEV_ERR_PLL_CALIB)) {
      ESP_LOGE(TAG,
               "Device errors after setup / bledy ukladu po inicjalizacji: 0x%04X%s%s. "
               "Reference or PLL did not come up - receive sensitivity is degraded.",
               errors, (errors & DEV_ERR_XOSC_START) ? " XOSC_START_ERR" : "",
               (errors & DEV_ERR_PLL_CALIB) ? " PLL_CALIB_ERR" : "");
    } else {
      ESP_LOGI(TAG, "Device errors after setup / bledy ukladu po inicjalizacji: 0x%04X", errors);
    }
  }

  this->restart_rx();
  ESP_LOGV(TAG, "SX1262 setup done");
}

// ---------------------------------------------------------------------------
// get_status_: GetStatus (0xC0), one byte, no arguments.
//
// Every SX126x SPI transaction returns the status byte, and cmd_read_() throws
// it away as protocol overhead - which is correct for every other command and
// exactly wrong for this one, where that byte is the entire response.
// ---------------------------------------------------------------------------
uint8_t SX1262::get_status_() {
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_GET_STATUS);
  const uint8_t status = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
  return status;
}

// SX126x GetStatus, bits 6:4. FS is the analogue of the SX1276 trap: the
// synthesiser is locked but the receiver is not running.
static const char *sx126x_chip_mode_name_(uint8_t status) {
  switch ((status >> 4) & 0x07) {
    case 0x02: return "STBY_RC";
    case 0x03: return "STBY_XOSC";
    case 0x04: return "FS";
    case 0x05: return "RX";
    case 0x06: return "TX";
    default: return "?";
  }
}

// ---------------------------------------------------------------------------
// dump_debug_status: re-read the receive chain while it is running.
//
// log_reg_status() runs once at boot and proves the chip answers over SPI.
// When a node stops receiving, every useful question is about the current
// state - is the radio in RX or did it stop in FS, has any IRQ latched since
// the last re-arm, did the sync word survive, is the boosted gain still set,
// has the chip logged a device error since boot - and a boot snapshot answers
// none of them.
//
// Called from Radio::receive_frame() on an interrupt timeout when diagnostics
// are verbose: roughly once a minute on a silent node, never on a busy one.
// ---------------------------------------------------------------------------
void SX1262::dump_debug_status(const char *reason) {
  const uint8_t status = this->get_status_();
  const uint16_t irq = this->get_irq_status_();

  uint8_t dev_err[2]{};
  this->cmd_read_(CMD_GET_DEVICE_ERRORS, {}, dev_err, sizeof(dev_err));
  const uint16_t errors = u16be_(dev_err);

  const uint8_t reg_rx_gain = this->read_register8_(REG_RX_GAIN);
  const uint8_t sync0 = this->read_register8_(REG_SYNC_WORD_0);
  const uint8_t sync1 = this->read_register8_(REG_SYNC_WORD_0 + 1);
  const uint8_t sync2 = this->read_register8_(REG_SYNC_WORD_0 + 2);
  const uint8_t rx_ptr = this->read_register8_(REG_RX_ADDR_PTR);
  const uint8_t pld_len = this->read_register8_(REG_RXTX_PAYLOAD_LEN);

  const int irq_level = (this->irq_pin_ != nullptr) ? (int) this->irq_pin_->digital_read() : -1;
  const int busy_level = (this->busy_pin_ != nullptr) ? (int) this->busy_pin_->digital_read() : -1;

  ESP_LOGI(TAG,
           "DEBUG [%s]: Status=0x%02X (%s) IrqStatus=0x%04X DeviceErrors=0x%04X RegRxGain=0x%02X "
           "Sync=%02X%02X%02X RxAddrPtr=0x%02X RxTxPldLen=0x%02X DIO1=%d BUSY=%d",
           reason != nullptr ? reason : "?", status, sx126x_chip_mode_name_(status), irq, errors,
           reg_rx_gain, sync0, sync1, sync2, rx_ptr, pld_len, irq_level, busy_level);

  const bool in_rx = ((status >> 4) & 0x07) == 0x05;
  ESP_LOGI(TAG, "DEBUG [%s]: sync_word_valid_latched=%s receiver_running=%s",
           reason != nullptr ? reason : "?",
           (irq & IRQ_SYNC_WORD_VALID) ? "yes" : "no", in_rx ? "yes" : "NO");

  if (!in_rx) {
    ESP_LOGW(TAG,
             "SX1262 is not in RX (mode=%s) / SX1262 nie jest w trybie RX. "
             "Nothing can be received in this state.",
             sx126x_chip_mode_name_(status));
  }
  if (errors & (DEV_ERR_XOSC_START | DEV_ERR_PLL_CALIB)) {
    ESP_LOGE(TAG, "SX1262 device errors 0x%04X%s%s / bledy ukladu - receive sensitivity is degraded.",
             errors, (errors & DEV_ERR_XOSC_START) ? " XOSC_START_ERR" : "",
             (errors & DEV_ERR_PLL_CALIB) ? " PLL_CALIB_ERR" : "");
  }
}

void SX1262::log_reg_status() {
  // TEST INSTRUMENTATION: print both snapshots here, when ESPHome emits the
  // normal visible component report. Capture the live state immediately before
  // printing it; logging itself performs no further SPI transactions.
  if (this->reset_register_snapshot_valid_) {
    this->log_candidate_registers_("after_reset", this->reset_register_snapshot_);
  }
  std::array<uint8_t, CANDIDATE_REGISTER_COUNT> configured_snapshot{};
  this->capture_candidate_registers_(configured_snapshot);
  this->log_candidate_registers_("rx_configured", configured_snapshot);

  // SX1262 uses 16-bit register addresses; reading via CMD_READ_REGISTER.
  const uint8_t reg_rx_gain  = this->read_register8_(REG_RX_GAIN);
  const uint8_t reg_sync0    = this->read_register8_(REG_SYNC_WORD_0);
  const uint8_t reg_sync1    = this->read_register8_(REG_SYNC_WORD_0 + 1);
  const uint8_t reg_sync2    = this->read_register8_(REG_SYNC_WORD_0 + 2);
  const uint8_t agc_cal_h    = this->read_register8_(REG_AGC_RSSI_MEAS_CAL_H);
  const uint8_t agc_cal_l    = this->read_register8_(REG_AGC_RSSI_MEAS_CAL_L);
  const uint8_t agc_tune12   = this->read_register8_(REG_AGC_GAIN_TUNE_1_2);
  const uint8_t agc_tune34   = this->read_register8_(REG_AGC_GAIN_TUNE_3_4);
  const uint8_t agc_tune56   = this->read_register8_(REG_AGC_GAIN_TUNE_5_6);
  const uint8_t agc_tune78   = this->read_register8_(REG_AGC_GAIN_TUNE_7_8);
  const uint8_t agc_tune910  = this->read_register8_(REG_AGC_GAIN_TUNE_9_10);
  const uint8_t agc_tune1112 = this->read_register8_(REG_AGC_GAIN_TUNE_11_12);
  const uint8_t agc_tune13   = this->read_register8_(REG_AGC_GAIN_TUNE_13);
  const uint8_t agc_first    = this->read_register8_(REG_AGC_FIRST_POWER_THRESHOLD);

  // sync_bits is printed because it is otherwise invisible from outside, and a
  // board running an unexpected value is exactly the kind of thing that gets
  // debugged for an hour before someone checks what was actually flashed.
  ESP_LOGI(TAG, "RegRxGain=0x%02X RegSyncWord[0]=0x%02X [1]=0x%02X [2]=0x%02X sync_bits=%u",
           reg_rx_gain, reg_sync0, reg_sync1, reg_sync2,
           (unsigned) S1_SYNC_WORD_BITS_OR_DEFAULT(this->listen_mode_));
  ESP_LOGI(TAG,
           "RSSI/AGC calibration snapshot: CalH=0x%02X CalL=0x%02X "
           "Tune12=0x%02X Tune34=0x%02X Tune56=0x%02X Tune78=0x%02X "
           "Tune910=0x%02X Tune1112=0x%02X Tune13=0x%02X FirstPow=0x%02X",
           agc_cal_h, agc_cal_l, agc_tune12, agc_tune34, agc_tune56,
           agc_tune78, agc_tune910, agc_tune1112, agc_tune13, agc_first);

  if (reg_rx_gain == 0x00 && reg_sync0 == 0x00 && reg_sync1 == 0x00 && reg_sync2 == 0x00) {
    ESP_LOGE(TAG, "SX1262 not responding over SPI / SX1262 nie odpowiada po SPI. "
                  "Check VCC/GND/SCK/MOSI/MISO/NSS/RESET.");
  }
}

// ---------------------------------------------------------------------------
// restart_rx: re-arm the receiver for the next WMBus frame.
//
// WMBus C-mode has two formats differing only in the second sync byte.
// We cycle 3:1 towards Format B (0x3D) as it is more common in practice,
// so both formats are caught without any user configuration.
// ---------------------------------------------------------------------------
void SX1262::restart_rx() {
  // Ping-pong between C-mode Block B (0x3D) and Block A (0xCD)
  // WMBus C-mode exists in two Format variants that differ only in the second
  // sync byte: Format A uses 0x3D, Format B uses 0xCD. Both are valid C1
  // transmissions and real devices may transmit Format A.
  // C1-only MUST cycle through both sync bytes exactly like `both` mode —
  // otherwise Format A packets are silently missed.
  // Bug: previous code hardcoded 0xCD (Format B only) in C1-only mode.
  // Fix: apply the same 3:1 (A:B) cycling used in `both` mode.
  // NOTE: do NOT revert C1-only to a fixed 0xCD; that breaks Format A.
  if (this->listen_mode_ == LISTEN_MODE_S1) {
    this->set_s1_sync_word_();
    this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});
    this->cmd_write_(CMD_SET_STANDBY, {STANDBY_XOSC});
    // Re-assert the S1 RX base on every re-arm. The stream reader uses the
    // packet start pointer returned by GetRxBufferStatus, but resetting the
    // base here also prevents state left by an earlier circular capture from
    // becoming the next packet's starting point.
    this->cmd_write_(CMD_SET_BUFFER_BASE_ADDRESS, {0x00, 0x00});
    // AN1200.53 initializes both the software index and the hidden payload-end
    // register before SetRx. capture_rx_stream_() starts its index at zero.
    this->write_register_(REG_RXTX_PAYLOAD_LEN, {0xFF});
    this->configure_irq_params_();
    this->cmd_write_(CMD_SET_RX, {0xFF, 0xFF, 0xFF});
    this->rx_loaded_ = false;
    this->rx_idx_ = 0;
    this->rx_len_ = 0;
    this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
    return;
  }

  uint8_t sync2;
  if (this->listen_mode_ == LISTEN_MODE_T1) {
    sync2 = 0x3D;
  } else if (this->listen_mode_ == LISTEN_MODE_C1) {
    // Cycle 3:1 (A:B) — same ratio as `both` mode.
    sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
    this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);
  } else {
    sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
    this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);
  }

  this->set_sync_word_(sync2);

  this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});
  this->cmd_write_(CMD_SET_STANDBY, {STANDBY_XOSC});

  // Adaptive long-packet mode may switch IRQ routing between fast RX_DONE-only
  // and long-stream SYNC_WORD_VALID+RX_DONE. Re-apply it each time RX is armed.
  this->configure_irq_params_();

  // RX continuous
  this->cmd_write_(CMD_SET_RX, {0xFF, 0xFF, 0xFF});

  this->rx_loaded_ = false;
  this->rx_idx_ = 0;
  this->rx_len_ = 0;

  // Drop the previous frame's level, exactly like the SX1276 driver does. A
  // frame whose RSSI cannot be sampled must report "not measured" instead of
  // silently inheriting an unrelated reading.
  this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
}


// ---------------------------------------------------------------------------
// read: called byte-by-byte by the receiver task.
// On first call after an IRQ: loads buffer (normal) or streams (long-packet).
// Subsequent calls return successive bytes from rx_buffer_ until exhausted.
// ---------------------------------------------------------------------------
optional<uint8_t> SX1262::read() {
  if (!this->rx_loaded_) {
    const bool use_long_stream = this->long_stream_active_() || this->listen_mode_ == LISTEN_MODE_S1;

    if (use_long_stream) {
      // Long stream is used only while adaptive hold is active (or S1, which is
      // always handled as a stream). Do not use it for normal short T1 packets.
      const uint16_t irq = this->get_irq_status_();
      if ((irq & (IRQ_SYNC_WORD_VALID | IRQ_RX_DONE | IRQ_TIMEOUT | IRQ_CRC_ERROR)) == 0) {
        return {};
      }
      ESP_LOGD(TAG, "IRQ=%04X, capturing RX stream (adaptive long GFSK)", irq);
      if (!this->capture_rx_stream_(irq))
        return {};
    } else {
      // Fast normal FIFO path. This is intentionally the default even when
      // long_gfsk_packets_ is true; long_gfsk_packets_ only enables the
      // adaptive fallback below when packets hit the FIFO edge.
      if (!this->irq_pin_->digital_read())
        return {};
      ESP_LOGD(TAG, "IRQ detected, loading buffer");
      if (!this->load_rx_buffer_())
        return {};

      if (this->long_gfsk_packets_ && this->rx_len_ >= 250) {
       // Hold must exceed the meter TX interval so the next transmission lands inside
       // the streaming window. Typical fast T1 meters transmit every ~30s; 45s gives margin.
	   this->long_stream_hold_until_ms_ = millis() + 45000UL;  // 45 seconds
        ESP_LOGW(TAG,
                 "SX1262 long-frame edge detected (payload_len=%u) -> enabling adaptive long-stream hold for 45 s",
                 (unsigned) this->rx_len_);
      }
    }
  }

  if (this->rx_idx_ < this->rx_len_)
    return this->rx_buffer_[this->rx_idx_++];
  return {};
}

// ---------------------------------------------------------------------------
// get_rssi: return RSSI (dBm) cached at packet capture time.
// In long-GFSK mode the radio enters standby after capture, so
// GetPacketStatus would return zeros if queried here.
// ---------------------------------------------------------------------------
int8_t SX1262::get_rssi() {
  // Return cached RSSI captured when the RX buffer was filled.
  // In long-GFSK mode we stop RX after capture, so live GetPacketStatus
  // would often read as 0.
  return this->last_rssi_dbm_;
}

// Live channel reading, unlike get_rssi() which returns the value cached at the
// last packet capture. read_rssi_inst_dbm_() already existed for internal use;
// this only exposes it through the transceiver interface.
bool SX1262::read_channel_rssi_dbm(int8_t *out) {
  const int8_t rssi = this->read_rssi_inst_dbm_();
  if (rssi == -127 || rssi >= 0)
    return false;
  if (out != nullptr)
    *out = rssi;
  return true;
}

const char *SX1262::get_name() { return TAG; }

}  // namespace wmbus_radio
}  // namespace esphome
