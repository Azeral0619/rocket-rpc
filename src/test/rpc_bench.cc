#include <cstdio>
#include "order.pb.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/rpc/rpc_server.h"
#include <memory>
#include <thread>
#include <chrono>

class OrderImpl : public Order {
public:
    void makeOrder(::google::protobuf::RpcController*, const ::makeOrderRequest* req,
                   ::makeOrderResponse* rsp, ::google::protobuf::Closure* done) override {
        rsp->set_ret_code(0); rsp->set_res_info("OK");
        rsp->set_order_id("ORD-" + std::to_string(req->price()));
        if (done) done->Run();
    }
};

int main() {
    setbuf(stdout, NULL);
    printf("1\n"); fflush(stdout);
    rocket::Logger::Options opts; opts.file_path = "/dev/null"; opts.flush_interval_ms = 500;
    rocket::Logger::getInstance().start(opts);
    printf("2\n"); fflush(stdout);
    auto addr = std::make_shared<rocket::IPNetAddr>("127.0.0.1", 12799);
    rocket::RpcServer server(addr, 4);
    printf("3\n"); fflush(stdout);
    server.registerService(std::make_shared<OrderImpl>());
    std::thread svr([&] { server.start(); });
    svr.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    printf("4\n"); fflush(stdout);

    auto ch = std::make_shared<rocket::RpcChannel>(addr,
        std::make_shared<rocket::RpcConnectionPool>(4));
    printf("5\n"); fflush(stdout);

    for (int i = 0; i < 3; ++i) {
        makeOrderRequest r; r.set_price(i); r.set_goods("t"); makeOrderResponse rs;
        auto c = rocket::NewRpcController(); c->SetTimeout(5000);
        int err = ch->CallMethodBlocking(Order::descriptor()->FindMethodByName("makeOrder"), c.get(), &r, &rs, 5000);
        printf("  #%d err=%d %s\n", i, err, rs.order_id().c_str()); fflush(stdout);
    }
    printf("OK\n"); fflush(stdout);
    _exit(0);
}
