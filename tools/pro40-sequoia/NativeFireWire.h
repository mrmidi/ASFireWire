#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int32_t pro40_open(uint64_t guid);
int32_t pro40_state(uint32_t *generation, uint16_t *node);
// Shared admission for sync and async reads; no device transaction. The caller
// must still bind the actual request to its captured generation.
int32_t pro40_admit_read(uint64_t address, uint32_t length);
int32_t pro40_read(uint32_t generation, uint64_t address, uint32_t length, void *bytes);
// Once per open session, after host DMA Stop: bounded restoration read reserve.
void pro40_begin_cleanup(void);
void pro40_close(void);
typedef struct {
    uint32_t selected, status, measured, enabled, capabilities;
    uint32_t owner_high, owner_low;
} Pro40ClockState;
#if defined(PRO40_CLOCK_CONTROL) || defined(PRO40_RESOURCE_PROBE)
int32_t pro40_clock_state(uint32_t generation, Pro40ClockState *state);
#endif
#ifdef PRO40_CLOCK_CONTROL
int32_t pro40_set_idle_rate(uint32_t generation, uint32_t hz);
#endif
#ifdef PRO40_RESOURCE_PROBE
void *pro40_native_interface(void);
#endif
#ifdef __cplusplus
}
#endif
