#include <arpa/inet.h>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <memory>
#include <string>

#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_client.h"

namespace {
constexpr int kTestPort = 12346;
constexpr std::size_t kBufferSize = 100;
} // namespace

void test_connect() {

    // 调用 conenct 连接 server
    // wirte 一个字符串
    // 等待 read 返回结果

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        ROCKET_LOG_ERROR("invalid fd {}", fd);
        exit(0);
    }

    sockaddr_in server_addr{};
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(kTestPort);
    inet_aton("127.0.0.1", &server_addr.sin_addr);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    int rt = connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));

    if (rt != 0) {
        ROCKET_LOG_ERROR("connect failed, errno: {}", errno);
        return;
    }

    ROCKET_LOG_DEBUG("connect success");

    std::string msg = "hello rocket!";

    const ssize_t write_rt = write(fd, msg.c_str(), msg.length());

    ROCKET_LOG_DEBUG("success write {} bytes, [{}]", write_rt, msg);

    std::array<char, kBufferSize> buf{};
    const ssize_t read_rt = read(fd, buf.data(), buf.size());
    ROCKET_LOG_DEBUG("success read {} bytes, [{}]", read_rt,
                     std::string(buf.data(), static_cast<std::size_t>(read_rt)));

    close(fd);
    ROCKET_LOG_DEBUG("close fd {}", fd);
}

void test_tcp_client() {

    rocket::EventLoop event_loop;

    auto addr = std::make_shared<rocket::IPNetAddr>("127.0.0.1", kTestPort);
    rocket::TcpClient client(addr);
    client.connect([addr, &client, &event_loop]() {
        ROCKET_LOG_DEBUG("connect to [{}] success", addr->toString());
        std::shared_ptr<rocket::TinyPBProtocol> message = std::make_shared<rocket::TinyPBProtocol>();
        message->m_msg_id = "123456789";
        message->m_method_name = "Test.Echo"; // 添加方法名以通过协议验证
        message->m_pb_data = "test pb data";
        client.writeMessage(message, [](const std::shared_ptr<rocket::AbstractProtocol>&) {
            ROCKET_LOG_DEBUG("send message success");
        });

        client.readMessage("123456789", [&event_loop](const std::shared_ptr<rocket::AbstractProtocol>& msg_ptr) {
            std::shared_ptr<rocket::TinyPBProtocol> message =
                std::dynamic_pointer_cast<rocket::TinyPBProtocol>(msg_ptr);
            ROCKET_LOG_DEBUG("msg_id[{}], get response {}", message->m_msg_id, message->m_pb_data);
            event_loop.stop();
        });
    });

    event_loop.loop();
}

int main(int argc, char* argv[]) {

    const char* config_file = "/home/azeral/code/rocket-rpc/config/client.yaml";
    if (argc == 2) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        config_file = argv[1];
    }

    rocket::Config::getInstance().reload(config_file);

    // test_connect();

    test_tcp_client();

    return 0;
}