#include "rocket/net/coder/string_coder.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_client.h"
#include "rocket/common/log.h"
#include <iostream>
#include <memory>

int main() {
    rocket::Logger::getInstance().start();

    // One-liner: connectSync blocks until connected.
    auto client = std::make_shared<rocket::TcpClient>(
        rocket::IPNetAddr::Make("127.0.0.1", 9999),
        [] { return std::make_unique<rocket::StringCoder>(); });

    if (int err = client->connectSync(); err != 0) {
        ROCKET_LOG_ERROR("Connect failed: {}", client->getConnectErrorInfo());
        return 1;
    }

    // Synchronous request/response.
    auto req = std::make_shared<rocket::StringProtocol>();
    req->m_msg_id = "string_msg";
    req->info = "hello from sync client";

    auto reply = client->requestSync(req);
    if (!reply) {
        ROCKET_LOG_ERROR("Request timed out");
        return 1;
    }

    auto* str = dynamic_cast<rocket::StringProtocol*>(reply.get());
    std::cout << "Response: " << (str ? str->info : "?") << "\n";

    return 0;
}
