#ifndef ASFW_DV_CAPTURE_ATOMICS_H
#define ASFW_DV_CAPTURE_ATOMICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t ASFWAtomicLoadU32Acquire(const volatile uint32_t* value);
void ASFWAtomicStoreU32Release(volatile uint32_t* value, uint32_t desired);

#ifdef __cplusplus
}
#endif

#endif
