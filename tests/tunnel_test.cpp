#include <pgain/tunnel.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <map>
#include <string>
#include <thread>

namespace {

pgain::Address loopback(uint16_t port = 0) { return pgain::parse_address("127.0.0.1:" + std::to_string(port)); }

pgain::Address loopback6(uint16_t port = 0) { return pgain::parse_address("[::1]:" + std::to_string(port)); }

std::span<const std::byte> as_bytes(const std::string& s) { return std::as_bytes(std::span(s)); }

std::string as_string(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

const pgain::Key key = pgain::parse_key(std::string(64, '7'));

void send_raw(const pgain::Fd& fd, const std::string& bytes) {
    pgain::SendBatch tx(1);
    tx.add(nullptr, as_bytes(bytes));
    tx.flush(fd);
}

std::string seal(uint32_t session, uint64_t seq, const std::string& payload, uint8_t path = 0) {
    std::array<std::byte, pgain::header_size> header;
    std::array<std::byte, pgain::tag_size> tag;
    pgain::encode({pgain::Type::data, path, session, seq}, header);
    pgain::Mac(key).sign(header, as_bytes(payload), tag);
    return as_string(header) + payload + as_string(tag);
}

std::vector<std::string> drain(const pgain::Fd& fd) {
    pgain::RecvBatch rx;
    std::vector<std::string> out;
    size_t n = rx.recv(fd);
    for (size_t i = 0; i < n; i++)
        out.push_back(as_string(rx.packet(i)));
    return out;
}

struct Rig {
    pgain::Fd echo = pgain::udp_bind(loopback());
    pgain::Exit exit{key, loopback(), loopback(pgain::local_port(echo))};
    pgain::Client client;
    pgain::Fd app = pgain::udp_connect(loopback(client.port()));
    std::map<std::string, int> echoed, replies;

    explicit Rig(size_t paths, const pgain::Key& client_key = key)
        : client(client_key, loopback(), std::vector<pgain::Address>(paths, loopback(exit.port()))) {}

    void send(const std::string& msg) { send_raw(app, msg); }

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
    send_raw(stranger, "not a tunnel packet");
    r.exit.poll(100);
    CHECK(r.exit.sessions() == 0);
}

TEST_CASE("client_with_wrong_key_gets_nothing_through") {
    Rig r(2, pgain::parse_key(std::string(64, '8')));
    r.send("ping");
    r.pump(1);
    CHECK(r.echoed.empty());
    CHECK(r.exit.sessions() == 0);
}

TEST_CASE("exit_drops_packet_with_any_byte_flipped") {
    Rig r(1);
    pgain::Fd fake = pgain::udp_connect(loopback(r.exit.port()));
    std::string good = seal(42, 0, "payload");
    for (size_t i = 0; i < good.size(); i++) {
        std::string bad = good;
        bad[i] ^= 1;
        send_raw(fake, bad);
    }
    r.exit.poll(100);
    CHECK(r.exit.sessions() == 0);
    send_raw(fake, good);
    r.exit.poll(100);
    CHECK(r.exit.sessions() == 1);
}

TEST_CASE("replay_from_new_address_does_not_steal_return_path") {
    Rig r(1);
    pgain::Fd owner = pgain::udp_connect(loopback(r.exit.port()));
    pgain::Fd thief = pgain::udp_connect(loopback(r.exit.port()));
    std::string first = seal(42, 0, "hello");
    send_raw(owner, first);
    pgain::RecvBatch rx;
    size_t n = 0;
    for (int i = 0; i < 100 && n == 0; i++) {
        r.exit.poll(10);
        n = rx.recv(r.echo);
    }
    REQUIRE(n == 1);
    pgain::Address upstream = rx.from(0);
    send_raw(thief, first);
    r.exit.poll(100);

    std::string push = "server push";
    pgain::SendBatch tx(1);
    tx.add(&upstream, as_bytes(push));
    tx.flush(r.echo);
    std::vector<std::string> got;
    for (int i = 0; i < 100 && got.empty(); i++) {
        r.exit.poll(10);
        got = drain(owner);
    }
    CHECK(got.size() == 1);
    CHECK(drain(thief).empty());
}

TEST_CASE("payload_round_trips_byte_for_byte") {
    Rig r(2);
    std::string big(pgain::max_payload, '\0');
    for (size_t i = 0; i < big.size(); i++)
        big[i] = char(i * 7);
    r.send(big);
    r.pump(1);
    CHECK(r.replies.count(big) == 1);
}

TEST_CASE("client_delivers_only_authentic_replies") {
    pgain::Fd fake_exit = pgain::udp_bind(loopback());
    pgain::Client client(key, loopback(), {loopback(pgain::local_port(fake_exit))});
    pgain::Fd app = pgain::udp_connect(loopback(client.port()));
    send_raw(app, "ping");
    pgain::RecvBatch rx;
    size_t n = 0;
    for (int i = 0; i < 100 && n == 0; i++) {
        client.poll(10);
        n = rx.recv(fake_exit);
    }
    REQUIRE(n >= 1);
    auto h = pgain::decode(rx.packet(0));
    REQUIRE(h);
    pgain::Address to = rx.from(0);

    std::string forged = seal(h->session, 0, "forged");
    forged.back() ^= 1;
    std::string real = seal(h->session, 1, "real");
    pgain::SendBatch tx(2);
    tx.add(&to, as_bytes(forged));
    tx.add(&to, as_bytes(real));
    tx.flush(fake_exit);
    std::vector<std::string> got;
    for (int i = 0; i < 100 && got.empty(); i++) {
        client.poll(10);
        got = drain(app);
    }
    client.poll(50);
    for (auto& m : drain(app))
        got.push_back(m);
    CHECK(got == std::vector<std::string>{"real"});
}

TEST_CASE("interleaved_sessions_reach_their_own_upstream") {
    Rig r(2);
    pgain::Client other(key, loopback(), {loopback(r.exit.port()), loopback(r.exit.port())});
    pgain::Fd other_app = pgain::udp_connect(loopback(other.port()));
    std::vector<std::string> other_got;
    for (int i = 0; i < 64; i++) {
        r.send("a" + std::to_string(i));
        send_raw(other_app, "b" + std::to_string(i));
        if (i % 8 != 7)
            continue;
        other.poll(0);
        r.pump(size_t(i + 1));
        for (int j = 0; j < 20; j++) {
            other.poll(0);
            r.exit.poll(0);
        }
        for (auto& m : drain(other_app))
            other_got.push_back(m);
    }
    CHECK(r.exit.sessions() == 2);
    CHECK(r.replies.size() == 64);
    for (auto& [msg, n] : r.replies)
        CHECK(msg[0] == 'a');
    CHECK(other_got.size() == 64);
    for (auto& msg : other_got)
        CHECK(msg[0] == 'b');
}

TEST_CASE("dual_stack_exit_serves_v4_and_v6_paths") {
    auto families = GENERATE(std::string("6"), std::string("4"), std::string("64"));
    pgain::Fd echo = pgain::udp_bind(loopback());
    pgain::Exit exit(key, pgain::parse_address("[::]:0"), loopback(pgain::local_port(echo)));
    std::vector<pgain::Address> paths;
    for (char f : families)
        paths.push_back(f == '6' ? loopback6(exit.port()) : loopback(exit.port()));
    pgain::Client client(key, loopback6(), paths);
    INFO("path families " << families);
    pgain::Fd app = pgain::udp_connect(loopback6(client.port()));
    send_raw(app, "ping");
    pgain::RecvBatch rx;
    std::vector<std::string> echoed, got;
    for (int i = 0; i < 200 && got.empty(); i++) {
        client.poll(1);
        exit.poll(1);
        size_t n = rx.recv(echo);
        pgain::SendBatch tx(pgain::batch_size);
        for (size_t j = 0; j < n; j++) {
            echoed.push_back(as_string(rx.packet(j)));
            tx.add(&rx.from(j), rx.packet(j));
        }
        tx.flush(echo);
        got = drain(app);
    }
    CHECK(echoed == std::vector<std::string>{"ping"});
    CHECK(got == std::vector<std::string>{"ping"});
}

TEST_CASE("exit_stops_replying_on_paths_the_client_abandoned") {
    Rig r(1);
    pgain::Fd a = pgain::udp_connect(loopback(r.exit.port()));
    pgain::Fd b = pgain::udp_connect(loopback(r.exit.port()));
    pgain::RecvBatch rx;
    auto bounce = [&] {
        size_t n = 0;
        for (int i = 0; i < 100 && n == 0; i++) {
            r.exit.poll(10);
            n = rx.recv(r.echo);
        }
        REQUIRE(n >= 1);
        pgain::SendBatch tx(pgain::batch_size);
        for (size_t i = 0; i < n; i++)
            tx.add(&rx.from(i), rx.packet(i));
        tx.flush(r.echo);
        for (int i = 0; i < 10; i++)
            r.exit.poll(10);
    };
    send_raw(a, seal(42, 0, "x", 0));
    send_raw(b, seal(42, 1, "y", 1));
    bounce();
    CHECK_FALSE(drain(a).empty());
    CHECK_FALSE(drain(b).empty());
    std::this_thread::sleep_for(pgain::path_idle + std::chrono::milliseconds(100));
    send_raw(a, seal(42, 2, "z", 0));
    bounce();
    CHECK(drain(a).size() == 1);
    CHECK(drain(b).empty());
}
