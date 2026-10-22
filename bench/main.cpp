#include "impair.hpp"

#include <pgain/tunnel.hpp>

#include <poll.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono;
using pgain::Address;

namespace {

struct Options {
    double loss = 0.1;
    double delay_ms = 10;
    double jitter_ms = 2;
    double rate = 128;
    double seconds = 10;
    size_t size = 64;
    size_t paths = 3;
    uint64_t seed = 1;
    uint64_t runs = 5;
};

Address loopback(uint16_t port = 0) { return pgain::parse_address("127.0.0.1:" + std::to_string(port)); }

int64_t now_ns() { return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count(); }

struct Cpu {
    double user = 0;
    double sys = 0;
    double total() const { return user + sys; }
};

Cpu thread_cpu() {
    rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    auto secs = [](timeval t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
    return {secs(ru.ru_utime), secs(ru.ru_stime)};
}

struct Echo {
    pgain::Fd fd = pgain::udp_bind(loopback());
    bool reply;
    std::atomic<uint64_t> received = 0;
    pgain::RecvBatch rx;

    explicit Echo(bool reply) : reply(reply) {}

    void poll(int timeout_ms) {
        pollfd p{fd.get(), POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0)
            return;
        size_t n = rx.recv(fd);
        received += n;
        if (!reply)
            return;
        pgain::SendBatch tx(pgain::batch_size);
        for (size_t i = 0; i < n; i++)
            tx.add(&rx.from(i), rx.packet(i));
        tx.flush(fd);
    }
};

struct Runner {
    std::vector<std::jthread> threads;
    template <class T>
    std::shared_ptr<Cpu> run(T& component) {
        auto spent = std::make_shared<Cpu>();
        threads.emplace_back([&component, spent](std::stop_token st) {
            while (!st.stop_requested())
                component.poll(1);
            *spent = thread_cpu();
        });
        return spent;
    }

    void stop() {
        for (auto& t : threads)
            t.request_stop();
        threads.clear();
    }
};

double percentile(std::vector<double>& v, double p) {
    if (v.empty())
        return NAN;
    std::sort(v.begin(), v.end());
    return v[size_t(p * double(v.size() - 1))];
}

struct Flow {
    uint64_t sent = 0;
    std::vector<double> rtt_us;
    std::vector<bool> got;
};

Flow paced_flow(const Address& entry, const Options& o, const std::function<void(uint64_t)>& before_send = {}) {
    pgain::Fd app = pgain::udp_connect(entry);
    std::map<uint64_t, int64_t> sent_at;
    std::mutex mu;
    Flow flow;
    flow.got.resize(uint64_t(o.rate * o.seconds));
    std::jthread receiver([&](std::stop_token st) {
        pgain::RecvBatch rx;
        while (!st.stop_requested()) {
            pollfd p{app.get(), POLLIN, 0};
            if (::poll(&p, 1, 5) <= 0)
                continue;
            size_t n = rx.recv(app);
            int64_t at = now_ns();
            std::lock_guard lock(mu);
            for (size_t i = 0; i < n; i++) {
                uint64_t seq;
                std::memcpy(&seq, rx.packet(i).data(), sizeof(seq));
                if (auto it = sent_at.find(seq); it != sent_at.end()) {
                    flow.rtt_us.push_back(double(at - it->second) / 1e3);
                    flow.got[seq] = true;
                    sent_at.erase(it);
                }
            }
        }
    });
    std::vector<std::byte> payload(std::max<size_t>(o.size, sizeof(uint64_t)));
    auto interval = duration<double>(1.0 / o.rate);
    auto start = steady_clock::now();
    auto total = uint64_t(o.rate * o.seconds);
    for (uint64_t seq = 0; seq < total; seq++) {
        std::this_thread::sleep_until(start + duration_cast<steady_clock::duration>(interval * double(seq)));
        if (before_send)
            before_send(seq);
        std::memcpy(payload.data(), &seq, sizeof(seq));
        {
            std::lock_guard lock(mu);
            sent_at[seq] = now_ns();
        }
        pgain::SendBatch tx(1);
        tx.add(nullptr, payload);
        tx.flush(app);
    }
    std::this_thread::sleep_for(milliseconds(500) + duration_cast<milliseconds>(duration<double, std::milli>(4 * (o.delay_ms + o.jitter_ms))));
    receiver.request_stop();
    receiver.join();
    flow.sent = total;
    return flow;
}

pgain::Impairment impairment(const Options& o) {
    return {o.loss, 0, duration_cast<microseconds>(duration<double, std::milli>(o.delay_ms)),
            duration_cast<microseconds>(duration<double, std::milli>(o.jitter_ms))};
}

void report(const char* setup, Flow& f, double expected) {
    double lost = 100.0 * double(f.sent - f.rtt_us.size()) / double(f.sent);
    double p50 = percentile(f.rtt_us, 0.5), p99 = percentile(f.rtt_us, 0.99);
    std::printf("%-10s %8llu %9.2f%% %9.2f%% %10.0f %10.0f\n", setup, (unsigned long long)f.sent, lost,
                100 * expected, p50, p99);
}

int loss_bench(const Options& o) {
    auto key = pgain::parse_key(pgain::generate_key());
    std::printf("loss %.1f%% per direction per path, delay %.1fms, jitter %.1fms, %.0f pps, %.0fs, seed %llu\n",
                100 * o.loss, o.delay_ms, o.jitter_ms, o.rate, o.seconds, (unsigned long long)o.seed);
    std::printf("%-10s %8s %10s %10s %10s %10s\n", "setup", "sent", "rt loss", "theory", "p50 us", "p99 us");
    {
        Echo echo(true);
        pgain::Impairer link(loopback(), loopback(pgain::local_port(echo.fd)), impairment(o), o.seed);
        Runner r;
        r.run(echo);
        r.run(link);
        auto f = paced_flow(loopback(link.port()), o);
        r.stop();
        report("direct", f, 1 - std::pow(1 - o.loss, 2));
    }
    for (size_t k = 1; k <= o.paths; k++) {
        Echo echo(true);
        pgain::Exit exit(key, loopback(), loopback(pgain::local_port(echo.fd)));
        std::vector<std::unique_ptr<pgain::Impairer>> links;
        std::vector<Address> paths;
        for (size_t i = 0; i < k; i++) {
            links.push_back(std::make_unique<pgain::Impairer>(loopback(), loopback(exit.port()), impairment(o),
                                                              o.seed * 1000 + k * 10 + i));
            paths.push_back(loopback(links.back()->port()));
        }
        pgain::Client client(key, loopback(), paths, k);
        Runner r;
        r.run(echo);
        r.run(exit);
        r.run(client);
        for (auto& l : links)
            r.run(*l);
        auto f = paced_flow(loopback(client.port()), o);
        r.stop();
        std::string name = std::to_string(k) + (k == 1 ? " path" : " paths");
        report(name.c_str(), f, 1 - std::pow(1 - std::pow(o.loss, double(k)), 2));
    }
    return 0;
}

int failover_bench(const Options& o) {
    auto key = pgain::parse_key(pgain::generate_key());
    std::printf("3 paths at %.0f/%.0f/%.0fms one-way, sending on 1, %.0f pps; path 0 cut halfway through\n",
                o.delay_ms, 2 * o.delay_ms, 3 * o.delay_ms, o.rate);
    std::printf("%-6s %10s %14s %16s\n", "run", "lost", "outage ms", "active after");
    for (uint64_t run = 0; run < o.runs; run++) {
        Echo echo(true);
        pgain::Exit exit(key, loopback(), loopback(pgain::local_port(echo.fd)));
        std::vector<std::unique_ptr<pgain::Impairer>> links;
        std::vector<Address> paths;
        for (int i = 1; i <= 3; i++) {
            auto delay = duration_cast<microseconds>(duration<double, std::milli>(o.delay_ms * i));
            links.push_back(std::make_unique<pgain::Impairer>(loopback(), loopback(exit.port()),
                                                              pgain::Impairment{0, 0, delay, {}}, o.seed + run));
            paths.push_back(loopback(links.back()->port()));
        }
        pgain::Client client(key, loopback(), paths, 1);
        Runner r;
        r.run(echo);
        r.run(exit);
        r.run(client);
        for (auto& l : links)
            r.run(*l);
        uint64_t cut = uint64_t(o.rate * o.seconds / 2);
        auto f = paced_flow(loopback(client.port()), o, [&](uint64_t seq) {
            if (seq == cut)
                links[0]->set_loss(1);
        });
        r.stop();
        uint64_t lost = 0, run_len = 0, longest = 0;
        for (uint64_t s = cut; s < f.sent; s++) {
            run_len = f.got[s] ? 0 : run_len + 1;
            longest = std::max(longest, run_len);
            lost += !f.got[s];
        }
        std::string active;
        for (size_t p : client.active())
            active += std::to_string(p) + " ";
        std::printf("%-6llu %10llu %14.0f %16s\n", (unsigned long long)run, (unsigned long long)lost,
                    1000.0 * double(longest) / o.rate, active.c_str());
    }
    return 0;
}

int relay_bench(const Options& o) {
    auto key = pgain::parse_key(pgain::generate_key());
    std::printf("one path through 0 to %zu relays, no impairment, %.0f pps, %.0fs\n", o.paths, o.rate, o.seconds);
    std::printf("%-10s %8s %10s %10s\n", "relays", "sent", "p50 us", "p99 us");
    for (size_t hops = 0; hops <= o.paths; hops++) {
        Echo echo(true);
        pgain::Exit exit(key, loopback(), loopback(pgain::local_port(echo.fd)));
        std::vector<std::unique_ptr<pgain::Relay>> relays;
        uint16_t next = exit.port();
        for (size_t h = 0; h < hops; h++) {
            relays.push_back(std::make_unique<pgain::Relay>(key, loopback(), loopback(next)));
            next = relays.back()->port();
        }
        pgain::Client client(key, loopback(), {loopback(next)}, 1);
        Runner r;
        r.run(echo);
        r.run(exit);
        r.run(client);
        for (auto& relay : relays)
            r.run(*relay);
        auto f = paced_flow(loopback(client.port()), o);
        r.stop();
        std::printf("%-10zu %8llu %10.0f %10.0f\n", hops, (unsigned long long)f.sent, percentile(f.rtt_us, 0.5),
                    percentile(f.rtt_us, 0.99));
    }
    return 0;
}

int throughput_bench(const Options& o) {
    auto key = pgain::parse_key(pgain::generate_key());
    Echo echo(false);
    pgain::Exit exit(key, loopback(), loopback(pgain::local_port(echo.fd)));
    pgain::Client client(key, loopback(), std::vector<Address>(o.paths, loopback(exit.port())), o.paths);
    Runner r;
    r.run(echo);
    auto exit_cpu = r.run(exit);
    auto client_cpu = r.run(client);
    pgain::Fd app = pgain::udp_connect(loopback(client.port()));
    std::vector<std::byte> payload(o.size);
    uint64_t sent = 0;
    auto start = steady_clock::now(), end = start + duration_cast<steady_clock::duration>(duration<double>(o.seconds));
    pgain::SendBatch tx(pgain::batch_size);
    while (steady_clock::now() < end) {
        for (size_t i = 0; i < pgain::batch_size; i++)
            tx.add(nullptr, payload);
        tx.flush(app);
        sent += pgain::batch_size;
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(milliseconds(200));
    double wall = duration<double>(steady_clock::now() - start).count();
    r.stop();
    uint64_t got = echo.received;
    std::printf("%zu-byte payloads, %zu path(s), %.1fs\n", o.size, o.paths, wall);
    std::printf("offered        %12.0f pps\n", double(sent) / wall);
    std::printf("delivered      %12.0f pps (%.1f%% of offered)\n", double(got) / wall, 100.0 * double(got) / double(sent));
    for (auto [name, cpu] : {std::pair{"client", *client_cpu}, std::pair{"exit", *exit_cpu}})
        std::printf("%-14s %12.0f delivered packets per cpu-second (%.2fs user, %.2fs sys, %.0f%% kernel)\n", name,
                    double(got) / cpu.total(), cpu.user, cpu.sys, 100 * cpu.sys / cpu.total());
    return 0;
}

int usage() {
    std::fprintf(stderr, "usage: pgain-bench loss [--loss P] [--delay-ms D] [--jitter-ms J] [--rate PPS] [--seconds S]\n"
                         "                        [--size B] [--paths K] [--seed N]\n"
                         "       pgain-bench throughput [--seconds S] [--size B] [--paths K]\n"
                         "       pgain-bench relay [--paths MAX_HOPS] [--rate PPS] [--seconds S]\n"
                         "       pgain-bench failover [--delay-ms D] [--rate PPS] [--seconds S] [--runs N]\n");
    return 2;
}

}

int main(int argc, char** argv) try {
    if (argc < 2)
        return usage();
    Options o;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string flag = argv[i];
        double v = std::stod(argv[i + 1]);
        if (flag == "--loss") o.loss = v;
        else if (flag == "--delay-ms") o.delay_ms = v;
        else if (flag == "--jitter-ms") o.jitter_ms = v;
        else if (flag == "--rate") o.rate = v;
        else if (flag == "--seconds") o.seconds = v;
        else if (flag == "--size") o.size = size_t(v);
        else if (flag == "--paths") o.paths = size_t(v);
        else if (flag == "--seed") o.seed = uint64_t(v);
        else if (flag == "--runs") o.runs = uint64_t(v);
        else return usage();
    }
    if (argc % 2)
        return usage();
    std::string_view mode = argv[1];
    if (mode == "loss")
        return loss_bench(o);
    if (mode == "throughput")
        return throughput_bench(o);
    if (mode == "relay")
        return relay_bench(o);
    if (mode == "failover")
        return failover_bench(o);
    return usage();
} catch (const std::exception& e) {
    std::fprintf(stderr, "pgain-bench: %s\n", e.what());
    return 1;
}
