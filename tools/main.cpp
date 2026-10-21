#include <pgain/tunnel.hpp>

#include <cstdio>
#include <string_view>

namespace {

int usage() {
    std::fprintf(stderr, "usage: pgain genkey\n"
                         "       pgain client <keyfile> <listen> <path>...\n"
                         "       pgain exit <keyfile> <listen> <target>\n");
    return 2;
}

}

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "genkey") {
        std::printf("%s\n", pgain::generate_key().c_str());
        return 0;
    }
    if (argc < 5)
        return usage();
    std::string_view mode = argv[1];
    auto key = pgain::load_key(argv[2]);
    auto listen = pgain::parse_address(argv[3]);
    if (mode == "client") {
        std::vector<sockaddr_in> paths;
        for (int i = 4; i < argc; i++)
            paths.push_back(pgain::parse_address(argv[i]));
        pgain::Client c(key, listen, paths);
        for (;;)
            c.poll(-1);
    }
    if (mode == "exit" && argc == 5) {
        pgain::Exit e(key, listen, pgain::parse_address(argv[4]));
        for (;;)
            e.poll(1000);
    }
    return usage();
} catch (const pgain::Error& e) {
    std::fprintf(stderr, "pgain: %s\n", e.what());
    return 1;
}
