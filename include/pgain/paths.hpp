#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace pgain {

using Clock = std::chrono::steady_clock;

inline constexpr auto probe_interval = std::chrono::milliseconds(50);

class PathMonitor {
public:
    void sent(uint64_t seq);
    void replied(uint64_t seq, Clock::time_point sent_at, Clock::time_point now);

    bool alive(Clock::time_point now) const;
    double loss() const;
    double score_ms() const;
    Clock::duration srtt() const { return srtt_; }

private:
    static constexpr uint64_t window = 64;
    static constexpr uint64_t in_flight = 10;

    std::array<bool, window> replied_{};
    uint64_t next_ = 0;
    bool measured_ = false;
    Clock::duration srtt_{};
    Clock::duration rttvar_{};
    Clock::time_point last_reply_{};
};

std::vector<size_t> choose(const std::vector<PathMonitor>& paths, size_t k, const std::vector<size_t>& active,
                           Clock::time_point now);

}
