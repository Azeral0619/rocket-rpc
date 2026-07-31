# rocket-rpc

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
