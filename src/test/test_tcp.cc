#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_server.h"
#include <memory>

namespace {
constexpr int kTestPort = 12346;
} // namespace

void test_tcp_server() {

    auto addr = std::make_shared<rocket::IPNetAddr>("127.0.0.1", kTestPort);

    ROCKET_LOG_DEBUG("create addr {}", addr->toString());

    rocket::TcpServer tcp_server(addr);

    tcp_server.start();
}

int main(int argc, char* argv[]) {
    const char* config_file = "/home/azeral/code/rocket-rpc/config/server.yaml";
    if (argc == 2) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        config_file = argv[1];
    }

    rocket::Config::getInstance().reload(config_file);

    test_tcp_server();

    return 0;
}