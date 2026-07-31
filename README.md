# rocket-rpc

## Typed C++ client

`MakeClient<Stub>` wraps the protobuf-generated stub and reuses the channel's
connection pool. The method pointer is a compile-time value, so request and
response types are inferred without runtime reflection:

```cpp
using namespace std::chrono_literals;

auto order = rocket::MakeClient<Order_Stub>("order_server");

auto result = co_await order.call<&Order_Stub::makeOrder>(
    std::move(request), {.timeout = 3s});
if (!result) {
    // result.status().code(), result.status().message()
}
```

The same generated method supports blocking and callback calls:

```cpp
auto blocking = order.callBlocking<&Order_Stub::makeOrder>(
    std::move(request), {.timeout = 3s});

order.callAsync<&Order_Stub::makeOrder>(
    std::move(request), {.timeout = 3s},
    [](rocket::RpcResult<makeOrderResponse> result) {
        // Handle completion.
    });
```

`Client<Stub>` is a small, copyable, thread-safe handle. Keep it as a service
member and share it across business threads. Requests are accepted by value so
asynchronous calls never retain references to caller-owned protobuf objects;
use `std::move(request)` to avoid a protobuf copy.

## RPC execution model

RPC methods run inline on their connection's IO thread by default. Keep inline
handlers non-blocking and short. Synchronous database calls, file IO, or other
blocking work can be isolated on the server's bounded worker pool:

```cpp
rocket::RpcServer server(address, 8);

// Apply to every registered method.
server.setDefaultExecutionMode(rocket::RpcExecutionMode::WorkerPool);

// Or keep the inline default and opt in only blocking methods.
server.setMethodExecutionMode(
    "package.Service.BlockingMethod",
    rocket::RpcExecutionMode::WorkerPool);
```

Execution modes must be configured before `RpcServer::start()`. The worker
queue defaults to 4096 pending calls; pass the fourth constructor argument to
change its capacity. A full queue fails fast with
`rocket::error::kRpcServerOverloaded` instead of growing without bound.
