// Shared-pool stress test: multiple worker threads share one RpcChannel
// (backed by one TcpClient connection).  Verifies the use-after-free fix
// and MPSC write queue under concurrent send.
#include "order.pb.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/rpc/rpc_server.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

class OrderImpl : public Order {
public:
    void makeOrder(::google::protobuf::RpcController*, const ::makeOrderRequest* req,
                   ::makeOrderResponse* rsp, ::google::protobuf::Closure* done) override {
        rsp->set_ret_code(0);
        rsp->set_res_info("OK");
        rsp->set_order_id("ORD-" + std::to_string(req->price()));
        if (done) done->Run();
    }
};

int main(int argc, char* argv[]) {
    int n_workers = argc > 1 ? std::atoi(argv[1]) : 8;
    int n_rounds  = argc > 2 ? std::atoi(argv[2]) : 1000;
    int timeout   = argc > 3 ? std::atoi(argv[3]) : 5000;

    setbuf(stdout, nullptr);

    rocket::Logger::Options opts;
    opts.file_path = "/dev/null";
    opts.flush_interval_ms = 500;
    rocket::Logger::getInstance().start(opts);

    auto addr = std::make_shared<rocket::IPNetAddr>("127.0.0.1", 12888);
    rocket::RpcServer server(addr, 4, false);
    server.registerService(std::make_shared<OrderImpl>());
    std::thread svr([&] { server.start(); });
    svr.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // All workers share one multi-flight channel.  The channel pins to one
    // TcpClient and routes concurrent responses by msg_id.
    auto pool = std::make_shared<rocket::RpcConnectionPool>(4);
    auto ch = std::make_shared<rocket::RpcChannel>(addr, pool);

    std::atomic<int> ok{0};
    std::atomic<int> err{0};
    auto t0 = std::chrono::steady_clock::now();

    auto do_work = [&](int tid) {
        for (int i = 0; i < n_rounds; ++i) {
            makeOrderRequest r;
            r.set_price(tid * n_rounds + i);
            r.set_goods("stress");
            makeOrderResponse rs;
            auto c = rocket::NewRpcController();
            c->SetTimeout(timeout);
            int e = ch->CallMethodBlocking(Order::descriptor()->FindMethodByName("makeOrder"),
                                           c.get(), &r, &rs, timeout);
            if (e == 0) ok.fetch_add(1);
            else err.fetch_add(1);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (int i = 0; i < n_workers; ++i) {
        workers.emplace_back(do_work, i);
    }
    for (auto& w : workers) w.join();

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    double sec = ms / 1000.0;
    int total = ok.load() + err.load();

    printf("\nworkers=%d rounds=%d total=%d ok=%d err=%d elapsed=%.1fs qps=%.0f\n",
           n_workers, n_rounds, total, ok.load(), err.load(), sec, total / sec);
    fflush(stdout);

    _exit(err > 0 ? 1 : 0);
}
