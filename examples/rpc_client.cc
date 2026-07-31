#include "order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_client.h"

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    if (argc > 1) {
        rocket::Config::getInstance().reload(argv[1]);
    }
    rocket::Logger::getInstance().start();

    auto order = rocket::MakeClient<Order_Stub>("order_server");

    makeOrderRequest request;
    request.set_price(100);
    request.set_goods("apple");

    auto result = order.callBlocking<&Order_Stub::makeOrder>(
        request, {.timeout = 3s});
    if (!result) {
        ROCKET_LOG_ERROR("RPC failed: code={}, message={}",
                         result.status().code(), result.status().message());
        return 1;
    }

    const auto& response = result.value();
    ROCKET_LOG_INFO("RPC success: ret_code={}, res_info={}, order_id={}",
                    response.ret_code(), response.res_info(), response.order_id());
    return 0;
}
