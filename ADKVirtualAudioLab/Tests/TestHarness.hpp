#pragma once

#include <cstdint>
#include <cstdio>

namespace ASFW::LabTests {

struct TestContext final {
    int checks{0};
    int failures{0};
};

} // namespace ASFW::LabTests

#define CHECK(ctx, cond)                                                       \
    do {                                                                       \
        ++(ctx).checks;                                                        \
        if (!(cond)) {                                                         \
            ++(ctx).failures;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                      \
    } while (0)

#define CHECK_EQ_U32(ctx, actual, expected)                                    \
    do {                                                                       \
        ++(ctx).checks;                                                        \
        const uint32_t a_ = static_cast<uint32_t>(actual);                     \
        const uint32_t e_ = static_cast<uint32_t>(expected);                   \
        if (a_ != e_) {                                                        \
            ++(ctx).failures;                                                  \
            std::printf("FAIL %s:%d  %s == 0x%08X, expected 0x%08X\n",         \
                        __FILE__, __LINE__, #actual, a_, e_);                  \
        }                                                                      \
    } while (0)

#define CHECK_EQ_U64(ctx, actual, expected)                                    \
    do {                                                                       \
        ++(ctx).checks;                                                        \
        const uint64_t a_ = static_cast<uint64_t>(actual);                     \
        const uint64_t e_ = static_cast<uint64_t>(expected);                   \
        if (a_ != e_) {                                                        \
            ++(ctx).failures;                                                  \
            std::printf("FAIL %s:%d  %s == %llu, expected %llu\n",             \
                        __FILE__, __LINE__, #actual,                           \
                        static_cast<unsigned long long>(a_),                   \
                        static_cast<unsigned long long>(e_));                  \
        }                                                                      \
    } while (0)
