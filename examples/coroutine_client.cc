// Example: using C++20 coroutines for RPC calls.
// Requires: xmake f -m release && xmake build coroutine_client
#include "order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/coroutine.h"
#include "rocket/net/rpc/rpc_client.h"

#include <chrono>
#include <iostream>
#include <memory>

using namespace std::chrono_literals;

// Coroutine: make three RPC calls sequentially, each awaiting the response.
rocket::Task<int> makeOrders(rocket::Client<Order_Stub> order) {
    makeOrderRequest req1;
    req1.set_price(100);
    req1.set_goods("apple");
    auto result1 = co_await order.call<&Order_Stub::makeOrder>(
        req1, {.timeout = 3s});
    if (!result1) co_return result1.status().code();
    std::cout << "Order 1: " << result1.value().order_id() << "\n";

    makeOrderRequest req2;
    req2.set_price(200);
    req2.set_goods("banana");
    auto result2 = co_await order.call<&Order_Stub::makeOrder>(
        req2, {.timeout = 3s});
    if (!result2) co_return result2.status().code();
    std::cout << "Order 2: " << result2.value().order_id() << "\n";

    makeOrderRequest req3;
    req3.set_price(300);
    req3.set_goods("cherry");
    auto result3 = co_await order.call<&Order_Stub::makeOrder>(
        req3, {.timeout = 3s});
    if (!result3) co_return result3.status().code();
    std::cout << "Order 3: " << result3.value().order_id() << "\n";

    co_return result3.value().ret_code();
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        rocket::Config::getInstance().reload(argv[1]);
    }
    rocket::Logger::getInstance().start();

    auto order = rocket::MakeClient<Order_Stub>("order_server");
    auto task = makeOrders(std::move(order));
    int ret = task.run();

    if (ret != 0) {
        ROCKET_LOG_ERROR("Coroutine RPC failed: {}", ret);
        return 1;
    }

    ROCKET_LOG_INFO("All orders placed successfully, ret={}", ret);
    return 0;
}
