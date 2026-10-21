#include <pgain/tunnel.hpp>

#include <cerrno>
#include <cstring>
#include <random>
#include <sys/epoll.h>

namespace pgain {

namespace {

[[noreturn]] void fail(const std::string& what, int err = errno) {
    throw Error(what + ": " + std::strerror(err));
}

Fd make_epoll() {
    Fd fd(epoll_create1(EPOLL_CLOEXEC));
    if (fd.get() < 0)
        fail("epoll_create1");
    return fd;
}

void watch(const Fd& epoll, const Fd& fd, epoll_data_t data) {
    epoll_event ev{EPOLLIN, data};
    if (epoll_ctl(epoll.get(), EPOLL_CTL_ADD, fd.get(), &ev) < 0)
        fail("epoll_ctl");
}

template <class F>
void wait(const Fd& epoll, int timeout_ms, F&& on_ready) {
    std::array<epoll_event, 64> evs;
    int n = epoll_wait(epoll.get(), evs.data(), int(evs.size()), timeout_ms);
    if (n < 0 && errno != EINTR)
        fail("epoll_wait");
    for (int i = 0; i < n; i++)
        on_ready(evs[i].data);
}

}

Client::Client(const sockaddr_in& listen, const std::vector<sockaddr_in>& paths)
    : epoll_(make_epoll()), app_(udp_bind(listen)), session_(std::random_device{}()) {
    if (paths.empty() || paths.size() > max_paths)
        throw Error("need 1 to " + std::to_string(max_paths) + " paths");
    watch(epoll_, app_, {.u64 = 0});
    for (auto& addr : paths) {
        paths_.push_back(udp_connect(addr));
        watch(epoll_, paths_.back(), {.u64 = paths_.size()});
    }
}

void Client::poll(int timeout_ms) {
    wait(epoll_, timeout_ms, [this](epoll_data_t d) { d.u64 == 0 ? from_app() : from_path(d.u64 - 1); });
}

void Client::from_app() {
    size_t n = app_rx_.recv(app_);
    if (n == 0)
        return;
    app_addr_ = app_rx_.from(n - 1);
    uint64_t next = seq_;
    for (size_t p = 0; p < paths_.size(); p++) {
        next = seq_;
        for (size_t i = 0; i < n; i++)
            if (auto pkt = app_rx_.packet(i); !pkt.empty())
                tx_.add(nullptr, {Type::data, uint8_t(p), session_, next++}, pkt);
        tx_.flush(paths_[p]);
    }
    seq_ = next;
}

void Client::from_path(size_t p) {
    size_t n = path_rx_.recv(paths_[p]);
    if (!app_addr_)
        return;
    for (size_t i = 0; i < n; i++) {
        auto pkt = path_rx_.packet(i);
        auto h = decode(pkt);
        if (h && h->session == session_ && dedup_.accept(h->seq))
            tx_.add(&*app_addr_, pkt.subspan(header_size));
    }
    tx_.flush(app_);
}

Exit::Exit(const sockaddr_in& listen, const sockaddr_in& target)
    : target_(target), epoll_(make_epoll()), listen_(udp_bind(listen)) {
    watch(epoll_, listen_, {.ptr = nullptr});
}

void Exit::poll(int timeout_ms) {
    wait(epoll_, timeout_ms, [this](epoll_data_t d) {
        d.ptr ? from_upstream(*static_cast<Session*>(d.ptr)) : from_tunnel();
    });
    expire(Clock::now());
}

Exit::Session* Exit::session(uint32_t id, Clock::time_point now) {
    if (auto it = sessions_.find(id); it != sessions_.end())
        return it->second.get();
    if (sessions_.size() >= max_sessions)
        return nullptr;
    auto s = std::make_unique<Session>(Session{id, udp_connect(target_), {}, 0, {}, now});
    watch(epoll_, s->upstream, {.ptr = s.get()});
    return sessions_.emplace(id, std::move(s)).first->second.get();
}

void Exit::from_tunnel() {
    size_t n = rx_.recv(listen_);
    auto now = Clock::now();
    for (size_t i = 0; i < n; i++) {
        auto pkt = rx_.packet(i);
        auto h = decode(pkt);
        if (!h || h->path >= max_paths)
            continue;
        Session* s = session(h->session, now);
        if (!s)
            continue;
        s->paths[h->path] = rx_.from(i);
        s->last_seen = now;
        if (s->dedup.accept(h->seq))
            send(s->upstream.get(), pkt.data() + header_size, pkt.size() - header_size, 0);
    }
}

void Exit::from_upstream(Session& s) {
    size_t n = upstream_rx_.recv(s.upstream);
    for (size_t i = 0; i < n; i++) {
        auto pkt = upstream_rx_.packet(i);
        if (pkt.empty())
            continue;
        uint64_t seq = s.seq++;
        for (size_t p = 0; p < max_paths; p++)
            if (s.paths[p])
                tx_.add(&*s.paths[p], {Type::data, uint8_t(p), s.id, seq}, pkt);
    }
    tx_.flush(listen_);
}

void Exit::expire(Clock::time_point now) {
    if (now - last_sweep_ < std::chrono::seconds(1))
        return;
    last_sweep_ = now;
    std::erase_if(sessions_, [now](auto& kv) { return now - kv.second->last_seen > session_idle; });
}

}
