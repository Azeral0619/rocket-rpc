#include "rocket/net/coder/string_coder.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_server.h"
#include "rocket/common/log.h"
#include <memory>

int main() {
    rocket::Logger::getInstance().start();

    // Simplified: use IPNetAddr::Make factory.
    rocket::TcpServer server(
        rocket::IPNetAddr::Make("127.0.0.1", 9999),
        [] { return std::make_unique<rocket::StringCoder>(); });

    server.setMessageCallback([](const rocket::TcpConnection::s_ptr& conn,
                                    std::vector<rocket::AbstractProtocol::s_ptr>& msgs) {
        for (auto& msg : msgs) {
            auto* str = dynamic_cast<rocket::StringProtocol*>(msg.get());
            if (!str) continue;
            auto reply = std::make_shared<rocket::StringProtocol>();
            reply->m_msg_id = str->m_msg_id;
            reply->info = "echo:" + str->info;
            conn->send(reply);
        }
    });

    ROCKET_LOG_INFO("Echo server on 127.0.0.1:9999");
    server.start();
    return 0;
}
