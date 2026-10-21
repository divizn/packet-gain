#pragma once

#include <array>
#include <cstdint>

namespace pgain {

class Dedup {
public:
    static constexpr uint64_t window = 1024;

    bool accept(uint64_t seq);

private:
    bool test(uint64_t seq) const { return bits_[seq % window / 64] >> (seq % 64) & 1; }
    void set(uint64_t seq) { bits_[seq % window / 64] |= uint64_t(1) << (seq % 64); }
    void clear(uint64_t seq) { bits_[seq % window / 64] &= ~(uint64_t(1) << (seq % 64)); }

    uint64_t next_ = 0;
    std::array<uint64_t, window / 64> bits_{};
};

}
