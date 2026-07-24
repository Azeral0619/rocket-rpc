// Example: using C++20 coroutines for RPC calls.
// Requires: xmake f -m release && xmake build coroutine_client
#include "proto/order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/coroutine.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"

#include <iostream>
#include <memory>

// Coroutine: make three RPC calls sequentially, each awaiting the response.
rocket::Task<int> makeOrders(rocket::RpcChannel::s_ptr channel) {
    auto* method = Order::descriptor()->FindMethodByName("makeOrder");

    makeOrderRequest req1;
    req1.set_price(100);
    req1.set_goods("apple");
    auto rsp1 = co_await rocket::coCall<makeOrderResponse>(channel, method, &req1);
    std::cout << "Order 1: " << rsp1.order_id() << "\n";

    makeOrderRequest req2;
    req2.set_price(200);
    req2.set_goods("banana");
    auto rsp2 = co_await rocket::coCall<makeOrderResponse>(channel, method, &req2);
    std::cout << "Order 2: " << rsp2.order_id() << "\n";

    makeOrderRequest req3;
    req3.set_price(300);
    req3.set_goods("cherry");
    auto rsp3 = co_await rocket::coCall<makeOrderResponse>(channel, method, &req3);
    std::cout << "Order 3: " << rsp3.order_id() << "\n";

    co_return rsp3.ret_code();
}

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

    auto task = makeOrders(channel);
    int ret = task.run();

    if (task.errorCode() != 0) {
        ROCKET_LOG_ERROR("Coroutine RPC failed: {}", task.errorInfo());
        return 1;
    }

    ROCKET_LOG_INFO("All orders placed successfully, ret={}", ret);
    return 0;
}
