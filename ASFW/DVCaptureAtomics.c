#include "DVCaptureAtomics.h"

uint32_t ASFWAtomicLoadU32Acquire(const volatile uint32_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void ASFWAtomicStoreU32Release(volatile uint32_t* value, uint32_t desired) {
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
}
