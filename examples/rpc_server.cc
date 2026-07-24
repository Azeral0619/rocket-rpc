#include "order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_server.h"

#include <memory>
#include <string>

// Simple Order service implementation.
class OrderServiceImpl : public Order {
  public:
    void makeOrder(::google::protobuf::RpcController* controller, const ::makeOrderRequest* request,
                   ::makeOrderResponse* response, ::google::protobuf::Closure* done) override {
        ROCKET_LOG_INFO("makeOrder: price={}, goods={}", request->price(), request->goods());

        response->set_ret_code(0);
        response->set_res_info("OK");
        response->set_order_id("ORDER-" + std::to_string(request->price()));

        if (done) {
            done->Run();
        }
    }
};

int main(int argc, char* argv[]) {
    // Load config from command line or default.
    if (argc > 1) {
        rocket::Config::getInstance().reload(argv[1]);
    }

    const auto cfg = rocket::Config::getInstance().getConfig();
    auto addr = std::make_shared<rocket::IPNetAddr>("127.0.0.1", static_cast<uint16_t>(cfg->port));
    rocket::Logger::getInstance().start();

    rocket::RpcServer server(addr);
    server.registerService(std::make_shared<OrderServiceImpl>());

    ROCKET_LOG_INFO("RpcServer starting on {}", addr->toString());
    server.start();
    return 0;
}
