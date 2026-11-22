// iris::newGuid (core/guid.h says the form).
#include "core/guid.h"

#include <cstdint>
#include <random>

namespace iris {

std::string newGuid()
{
    // One engine per thread, seeded from the system's entropy (four words: a single 32-bit seed would let two threads
    // draw the same stream with a probability no id should have).
    thread_local std::mt19937_64 engine = [] {
        std::random_device rd;
        std::seed_seq seq{ rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
        return std::mt19937_64(seq);
    }();
    std::uint64_t hi = engine(), lo = engine();
    hi = (hi & ~std::uint64_t(0xF000)) | std::uint64_t(0x4000);                 // the version nibble: 4
    lo = (lo & ~(std::uint64_t(0xC) << 60)) | (std::uint64_t(0x8) << 60);       // the variant: 10xx (8, 9, a, b)
    static const char kHex[] = "0123456789abcdef";
    std::string out(36, '-');
    int k = 0;
    for (int i = 0; i < 32; ++i) {
        if (k == 8 || k == 13 || k == 18 || k == 23) ++k;
        const std::uint64_t word = i < 16 ? hi : lo;
        out[k++] = kHex[(word >> (60 - 4 * (i % 16))) & 0xF];
    }
    return out;
}

}   // namespace iris
