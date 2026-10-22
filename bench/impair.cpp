#include "impair.hpp"

#include <poll.h>

namespace pgain {

Impairer::Impairer(const Address& listen, const Address& target, const Impairment& imp, uint64_t seed)
    : imp_(imp), rng_(seed), down_(udp_bind(listen)), up_(udp_connect(target)) {}

void Impairer::poll(int timeout_ms) {
    if (!queue_.empty()) {
        auto wait = std::chrono::ceil<std::chrono::milliseconds>(queue_.top().due - Clock::now()).count();
        if (timeout_ms < 0 || wait < timeout_ms)
            timeout_ms = int(std::max<int64_t>(wait, 0));
    }
    pollfd fds[] = {{down_.get(), POLLIN, 0}, {up_.get(), POLLIN, 0}};
    ::poll(fds, 2, timeout_ms);
    if (fds[0].revents & POLLIN) {
        size_t n = rx_.recv(down_);
        if (n)
            peer_ = rx_.from(n - 1);
        admit(rx_, n, true);
    }
    if (fds[1].revents & POLLIN)
        admit(rx_, rx_.recv(up_), false);
    release();
}

void Impairer::admit(const RecvBatch& rx, size_t n, bool upward) {
    std::uniform_real_distribution<double> coin(0, 1);
    std::uniform_int_distribution<int64_t> jitter(0, imp_.jitter.count());
    auto now = Clock::now();
    for (size_t i = 0; i < n; i++) {
        auto pkt = rx.packet(i);
        if (coin(rng_) < imp_.loss)
            continue;
        int copies = coin(rng_) < imp_.duplicate ? 2 : 1;
        for (int c = 0; c < copies; c++)
            queue_.push({now + imp_.delay + std::chrono::microseconds(jitter(rng_)), upward,
                         {pkt.begin(), pkt.end()}});
    }
}

void Impairer::release() {
    SendBatch up(batch_size), down(batch_size);
    std::vector<Pending> sending;
    auto now = Clock::now();
    while (!queue_.empty() && queue_.top().due <= now && sending.size() < batch_size) {
        sending.push_back(queue_.top());
        queue_.pop();
    }
    for (auto& p : sending) {
        if (p.upward)
            up.add(nullptr, p.bytes);
        else if (peer_)
            down.add(&*peer_, p.bytes);
    }
    up.flush(up_);
    down.flush(down_);
}

}
