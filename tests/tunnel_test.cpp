#include <pgain/tunnel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <map>
#include <string>

namespace {

sockaddr_in loopback(uint16_t port = 0) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

std::span<const std::byte> as_bytes(const std::string& s) { return std::as_bytes(std::span(s)); }

std::string as_string(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

struct Rig {
    pgain::Fd echo = pgain::udp_bind(loopback());
    pgain::Exit exit{loopback(), loopback(pgain::local_port(echo))};
    pgain::Client client;
    pgain::Fd app = pgain::udp_connect(loopback(client.port()));
    std::map<std::string, int> echoed, replies;

    explicit Rig(size_t paths)
        : client(loopback(), std::vector<sockaddr_in>(paths, loopback(exit.port()))) {}

    void send(const std::string& msg) {
        pgain::SendBatch tx(1);
        tx.add(nullptr, as_bytes(msg));
        tx.flush(app);
    }

    void pump(size_t want) {
        pgain::RecvBatch rx;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (replies.size() < want && std::chrono::steady_clock::now() < deadline) {
            client.poll(0);
            exit.poll(0);
            size_t n = rx.recv(echo);
            pgain::SendBatch tx(pgain::batch_size);
            for (size_t i = 0; i < n; i++) {
                echoed[as_string(rx.packet(i))]++;
                tx.add(&rx.from(i), rx.packet(i));
            }
            tx.flush(echo);
            n = rx.recv(app);
            for (size_t i = 0; i < n; i++)
                replies[as_string(rx.packet(i))]++;
        }
        for (int i = 0; i < 20; i++) {
            client.poll(0);
            exit.poll(0);
        }
        size_t n = rx.recv(app);
        for (size_t i = 0; i < n; i++)
            replies[as_string(rx.packet(i))]++;
    }
};

}

TEST_CASE("round_trip_over_each_path_count_delivers_every_packet_once") {
    for (size_t paths : {1, 2, 4}) {
        Rig r(paths);
        for (int i = 0; i < 200; i++) {
            r.send("ping " + std::to_string(i));
            if (i % 8 == 7)
                r.pump(size_t(i + 1));
        }
        INFO(paths << " paths");
        CHECK(r.echoed.size() == 200);
        CHECK(r.replies.size() == 200);
        for (auto& [msg, n] : r.echoed)
            CHECK(n == 1);
        for (auto& [msg, n] : r.replies)
            CHECK(n == 1);
        CHECK(r.exit.sessions() == 1);
    }
}

TEST_CASE("exit_ignores_garbage_without_creating_sessions") {
    Rig r(1);
    pgain::Fd stranger = pgain::udp_connect(loopback(r.exit.port()));
    std::string garbage = "not a tunnel packet";
    pgain::SendBatch tx(1);
    tx.add(nullptr, as_bytes(garbage));
    tx.flush(stranger);
    r.exit.poll(100);
    CHECK(r.exit.sessions() == 0);
}

TEST_CASE("payload_round_trips_byte_for_byte") {
    Rig r(2);
    std::string big(pgain::max_packet - pgain::header_size, '\0');
    for (size_t i = 0; i < big.size(); i++)
        big[i] = char(i * 7);
    r.send(big);
    r.pump(1);
    CHECK(r.replies.count(big) == 1);
}
