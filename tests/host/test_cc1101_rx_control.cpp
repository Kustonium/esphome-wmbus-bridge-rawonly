#include "cc1101_rx_control.h"
#include <cassert>
#include <cstdint>
#include <vector>

using namespace esphome::wmbus_radio;

int main() {
  // Calibration transitions and a transient false RX observation must not
  // satisfy the wait until two consecutive samples confirm RX.
  std::vector<uint8_t> states{0x08, 0x0D, 0x0C, 0x0D, 0x0D};
  unsigned reads = 0;
  int64_t now = 0;
  assert(cc1101_wait_state([&]() { return states.at(reads++); }, [&]() { return now; },
                           [&]() { now += 20; }, 0x0D, 10000));
  assert(reads == 5);

  // Never spin forever on a disconnected chip or on a chip stuck in RX
  // while waiting for IDLE. The caller must not flush after this failure.
  for (uint8_t stuck : {uint8_t(0xFF), uint8_t(0x0D)}) {
    now = 0; reads = 0;
    assert(!cc1101_wait_state([&]() { ++reads; return stuck; }, [&]() { return now; },
                             [&]() { now += 20; }, 0x01, 10000));
    assert(now == 10000 && reads == 500);
  }
  // Reserved upper bits do not change the state number.
  now = 0;
  assert(cc1101_wait_state([]() { return uint8_t(0xE1); }, [&]() { return now; },
                           [&]() { now += 20; }, 0x01, 10000));

  assert(cc1101_idle_refresh_safe([]() { return false; }, []() { return 0; }, []() { return 0x0D; }));
  assert(!cc1101_idle_refresh_safe([]() { return true; }, []() { return 0; }, []() { return 0x0D; }));
  for (int bytes : {1, 16, 0x80})
    assert(!cc1101_idle_refresh_safe([]() { return false; }, [=]() { return bytes; }, []() { return 0x0D; }));
  for (int state : {0x01, 0x08, 0x0E, 0x11, 0xFF})
    assert(!cc1101_idle_refresh_safe([]() { return false; }, []() { return 0; }, [=]() { return state; }));
  // Packet starts during the guard: data on the second sample, or sync just
  // before the final decision, must suppress the refresh.
  unsigned samples = 0;
  assert(!cc1101_idle_refresh_safe([]() { return false; }, [&]() { return samples++ == 0 ? 0 : 1; },
                                  []() { return 0x0D; }));
  samples = 0;
  assert(!cc1101_idle_refresh_safe([&]() { return ++samples == 3; }, []() { return 0; },
                                  []() { return 0x0D; }));
}
