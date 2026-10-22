#include "impair.hpp"

#include <pgain/tunnel.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <memory>
#include <set>
#include <string>

namespace {

pgain::Address loopback(uint16_t port = 0) { return pgain::parse_address("127.0.0.1:" + std::to_string(port)); }

double one_way_loss(size_t paths, double loss, int packets) {
    auto key = pgain::parse_key(std::string(64, '5'));
    pgain::Fd echo = pgain::udp_bind(loopback());
    pgain::Exit exit(key, loopback(), loopback(pgain::local_port(echo)));
    std::vector<std::unique_ptr<pgain::Impairer>> links;
    std::vector<pgain::Address> addrs;
    for (size_t i = 0; i < paths; i++) {
        links.push_back(std::make_unique<pgain::Impairer>(loopback(), loopback(exit.port()),
                                                          pgain::Impairment{loss, 0.05, {}, {}}, 7 + i));
        addrs.push_back(loopback(links.back()->port()));
    }
    pgain::Client client(key, loopback(), addrs);
    pgain::Fd app = pgain::udp_connect(loopback(client.port()));
    pgain::RecvBatch rx;
    std::set<std::string> seen;
    int delivered = 0;
    auto pump = [&] {
        client.poll(0);
        for (auto& l : links)
            l->poll(0);
        exit.poll(0);
        size_t n = rx.recv(echo);
        for (size_t i = 0; i < n; i++) {
            auto p = rx.packet(i);
            delivered++;
            seen.emplace(reinterpret_cast<const char*>(p.data()), p.size());
        }
    };
    for (int i = 0; i < packets; i++) {
        std::string msg = std::to_string(i);
        pgain::SendBatch tx(1);
        tx.add(nullptr, std::as_bytes(std::span(msg)));
        tx.flush(app);
        if (i % 8 == 7)
            for (int j = 0; j < 10; j++)
                pump();
    }
    for (int j = 0; j < 100; j++)
        pump();
    REQUIRE(delivered == int(seen.size()));
    return 1 - double(seen.size()) / packets;
}

}

TEST_CASE("multipath_cuts_one_way_loss_to_about_p_to_the_k") {
    auto paths = GENERATE(size_t(1), size_t(2), size_t(3));
    double p = 0.2, expected = std::pow(p, double(paths));
    double measured = one_way_loss(paths, p, 3000);
    INFO(paths << " paths, measured " << measured << ", expected " << expected);
    CHECK(std::abs(measured - expected) < 4 * std::sqrt(expected * (1 - expected) / 3000) + 0.002);
}
