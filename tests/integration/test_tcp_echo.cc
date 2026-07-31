#include "rocket/net/coder/string_coder.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_client.h"
#include "rocket/net/tcp/tcp_server.h"
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

namespace rocket {
namespace {

TEST(TcpEcho, Roundtrip) {
    const auto addr = std::make_shared<IPNetAddr>("127.0.0.1", 0);
    auto server = std::make_unique<TcpServer>(addr, [] { return std::make_unique<StringCoder>(); });

    std::atomic<bool> server_received{false};
    server->setMessageCallback([&server_received](const TcpConnection::s_ptr& conn, std::vector<AbstractProtocol::s_ptr>& msgs) {
        for (auto& msg : msgs) {
            auto* str = dynamic_cast<StringProtocol*>(msg.get());
            ASSERT_NE(str, nullptr);
            server_received = true;

            auto reply = std::make_shared<StringProtocol>();
            reply->m_msg_id = str->m_msg_id;
            reply->info = "echo:" + str->info;
            conn->send(reply);
        }
    });

    std::promise<void> server_ready;
    server->setReadyCallback([&server_ready] { server_ready.set_value(); });

    std::jthread server_thread([&server] { server->start(); });

    server_ready.get_future().wait();
    const auto listen_addr = server->getListenAddr();
    ASSERT_NE(listen_addr, nullptr);
    const auto* ip_addr = dynamic_cast<const IPNetAddr*>(listen_addr.get());
    ASSERT_NE(ip_addr, nullptr);
    const int port = static_cast<int>(ip_addr->getPort());
    ASSERT_GT(port, 0);

    auto client = std::make_shared<TcpClient>(std::make_shared<IPNetAddr>("127.0.0.1", port),
                                               [] { return std::make_unique<StringCoder>(); });

    std::promise<void> connect_done;
    client->connect([&connect_done] { connect_done.set_value(); });
    connect_done.get_future().wait();
    EXPECT_EQ(client->getConnectErrorCode(), 0);

    std::promise<std::string> response_promise;
    auto req = std::make_shared<StringProtocol>();
    req->m_msg_id = 1;
    req->info = "hello";

    client->readMessage(1, [&response_promise](AbstractProtocol::s_ptr msg) mutable {
        auto* str = dynamic_cast<StringProtocol*>(msg.get());
        if (str != nullptr) {
            response_promise.set_value(str->info);
        } else {
            response_promise.set_value("");
        }
    });

    client->writeMessage(req, nullptr);

    auto future = response_promise.get_future();
    const auto status = future.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(status, std::future_status::ready);
    if (status == std::future_status::ready) {
        EXPECT_EQ(future.get(), "echo:hello");
    }
    EXPECT_TRUE(server_received.load());

    client.reset();
    server->stop();
    server_thread.join();
}

} // namespace
} // namespace rocket
