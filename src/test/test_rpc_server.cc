#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>
#include <memory>

#include "order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_dispatcher.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_server.h"

namespace {
constexpr int kMinPrice = 10;
} // namespace

class OrderImpl : public Order {
  public:
    void makeOrder(google::protobuf::RpcController* /*controller*/, const ::makeOrderRequest* request,
                   ::makeOrderResponse* response, ::google::protobuf::Closure* done) override {
        ROCKET_LOG_INFO("recv makeOrder: goods=[{}], price=[{}]", request->goods(), request->price());

        if (request->price() < kMinPrice) {
            response->set_ret_code(-1);
            response->set_res_info("short balance");
        } else {
            response->set_ret_code(0);
            response->set_res_info("success");
            response->set_order_id("20230514");
        }

        if (done != nullptr) {
            done->Run();
        }
    }
};

int main(int argc, char* argv[]) {

    const char* config_file = "/home/azeral/code/rocket-rpc/config/server.yaml";
    if (argc == 2) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        config_file = argv[1];
    }

    rocket::Config::getInstance().reload(config_file);

    std::shared_ptr<OrderImpl> service = std::make_shared<OrderImpl>();
    rocket::RpcDispatcher::getInstance().registerService(service);

    const auto config = rocket::Config::getInstance().getConfig();
    auto addr = std::make_shared<rocket::IPNetAddr>("127.0.0.1", config->port);

    rocket::TcpServer tcp_server(addr);

    tcp_server.start();

    return 0;
}