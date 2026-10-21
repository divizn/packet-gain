#include <pgain/tunnel.hpp>

#include <cstdio>
#include <string_view>

namespace {

int usage() {
    std::fprintf(stderr, "usage: pgain client <listen> <path>...\n"
                         "       pgain exit <listen> <target>\n");
    return 2;
}

}

int main(int argc, char** argv) try {
    if (argc < 4)
        return usage();
    std::string_view mode = argv[1];
    auto listen = pgain::parse_address(argv[2]);
    if (mode == "client") {
        std::vector<sockaddr_in> paths;
        for (int i = 3; i < argc; i++)
            paths.push_back(pgain::parse_address(argv[i]));
        pgain::Client c(listen, paths);
        for (;;)
            c.poll(-1);
    }
    if (mode == "exit" && argc == 4) {
        pgain::Exit e(listen, pgain::parse_address(argv[3]));
        for (;;)
            e.poll(1000);
    }
    return usage();
} catch (const pgain::Error& e) {
    std::fprintf(stderr, "pgain: %s\n", e.what());
    return 1;
}
