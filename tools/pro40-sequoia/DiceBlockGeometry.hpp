// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40 {
constexpr bool validDiceBlock(uint64_t offset,uint64_t size,uint64_t minimum) {
    return offset>=40 && !(offset&3) && !(size&3) && size>=minimum &&
        offset<=65536 && size<=65536-offset;
}
constexpr bool diceBlocksOverlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn) {
    // Call only after both block extents have passed validDiceBlock.
    return a<b+bn && b<a+an;
}
}
