#include "order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"

#include <memory>

int main(int argc, char* argv[]) {
    if (argc > 1) {
        rocket::Config::getInstance().reload(argv[1]);
    }
    rocket::Logger::getInstance().start();

    auto channel = rocket::NewRpcChannel("order_server");
    if (!channel) {
        ROCKET_LOG_ERROR("Failed to create RpcChannel");
        return 1;
    }

    makeOrderRequest request;
    request.set_price(100);
    request.set_goods("apple");

    makeOrderResponse response;
    rocket::RpcController controller;

    int err = channel->CallMethodBlocking(
        Order::descriptor()->FindMethodByName("makeOrder"),
        &controller, &request, &response, 3000);

    if (err != 0 || controller.Failed()) {
        ROCKET_LOG_ERROR("RPC failed: {}", controller.ErrorText());
        return 1;
    }

    ROCKET_LOG_INFO("RPC success: ret_code={}, res_info={}, order_id={}",
                    response.ret_code(), response.res_info(), response.order_id());
    return 0;
}
