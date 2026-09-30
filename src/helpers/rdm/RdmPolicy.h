#pragma once

#include "RdmConfig.h"

#include <stdint.h>

// Timing rules shared by Outbox and Fetcher. Each helper takes the caller's random32() value instead of drawing
// one, so every caller keeps exactly one draw per decision and seeded test sequences stay the same.

namespace rdm {

// G17a: every repeating interval I becomes I + U[0, jitterMax(I)], so timers of different nodes do not stay in step
inline uint32_t jitterMax(uint32_t interval) {
  return interval / RDM_JITTER_DIV < RDM_JITTER_MAX_S ? interval / RDM_JITTER_DIV : RDM_JITTER_MAX_S;
}

inline uint32_t jittered(uint32_t interval, uint32_t rnd) { return interval + rnd % (jitterMax(interval) + 1); }

// G16 first RETRY action, G6 boot kick and G17b repeat: 60-120 s
inline uint32_t firstRetryDelay(uint32_t rnd) {
  return RDM_RETRY_FIRST_MIN_S + rnd % (RDM_RETRY_FIRST_MAX_S - RDM_RETRY_FIRST_MIN_S + 1);
}

// G3, G9: max(2 x est_timeout, 30 s), rounded up to whole seconds
inline uint32_t waitSecs(uint32_t est_timeout_ms) {
  uint32_t s = (uint32_t)(((uint64_t)est_timeout_ms * 2 + 999) / 1000);
  return s > RDM_WAIT_ACK_MIN_S ? s : RDM_WAIT_ACK_MIN_S;
}

}
