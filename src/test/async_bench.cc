// True async benchmark: all in-flight lanes are driven by IO-thread callbacks.
// No per-request blocking threads — one callback fires the next request.
//
// Usage: async_bench [connections] [duration_s] [pipeline] [timeout_ms]
//                    [server_io] [server_workers] [payload_bytes]
// server_workers=0 keeps the inline IO-thread fast path (default).
#include "order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/rpc/rpc_server.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

class LatencyRecorder {
  public:
    void record(int64_t us) { std::lock_guard lk(m_mutex); m_samples.push_back(us); }
    void print() {
        std::lock_guard lk(m_mutex);
        if (m_samples.empty()) return;
        std::sort(m_samples.begin(), m_samples.end());
        printf("  p50=%.3fms  p99=%.3fms  max=%.3fms  samples=%zu\n",
               m_samples[m_samples.size()*50/100]/1000.0,
               m_samples[m_samples.size()*99/100]/1000.0,
               static_cast<double>(m_samples.back())/1000.0,
               m_samples.size());
    }
  private:
    std::mutex m_mutex;
    std::vector<int64_t> m_samples;
};

class OrderImpl : public Order {
  public:
    void makeOrder(::google::protobuf::RpcController*, const ::makeOrderRequest* req,
                   ::makeOrderResponse* rsp, ::google::protobuf::Closure* done) override {
        rsp->set_ret_code(0);
        rsp->set_res_info(req->goods());
        rsp->set_order_id("ORD-" + std::to_string(req->price()));
        if (done) done->Run();
    }
};

struct BenchState;

// Thread-local BenchCallback pool (Hical MpscNodePool pattern).
struct BenchCallback;

struct CbSlot { CbSlot* next; };
inline thread_local CbSlot* t_cb_head = nullptr;
inline thread_local size_t t_cb_count = 0;
constexpr size_t kCbPoolMax = 256;
constexpr int64_t kLatencySampleMask = 255;  // sample 1/256 completions

BenchCallback* allocCallback();

// Per-channel data.  Lives in a stable vector owned by BenchState.
// Reuses req/rsp/ctrl across requests (Hical coroutine-frame pattern).
struct Chan {
    std::shared_ptr<rocket::RpcChannel> ch;
    const google::protobuf::MethodDescriptor* method;
    int timeout_ms;
    // Reused across requests — Clear()/Reset() instead of NewMessage/NewController.
    std::shared_ptr<makeOrderRequest>  req;
    std::shared_ptr<makeOrderResponse> rsp;
    std::shared_ptr<rocket::RpcController> ctrl;
};

struct BenchState {
    std::atomic<bool> stop{false};
    std::atomic<int64_t> total{0};
    std::atomic<int64_t> errors{0};
    LatencyRecorder latency;
    std::vector<Chan> chans;  // stable after init, indexed by cid
};

// Callback carries the SEND timestamp of the request that just completed.
// Pooled via thread_local free list (Hical MpscNodePool pattern).
// Chan::req/rsp/ctrl are reused across requests (Clear/Reset, no alloc).
struct BenchCallback : public google::protobuf::Closure {
    BenchState* state;
    int cid;
    std::chrono::steady_clock::time_point t_send;

    BenchCallback() = default;

    void init(BenchState* s, int id, std::chrono::steady_clock::time_point send_time) {
        state = s; cid = id; t_send = send_time;
    }

    void Run() override {
        Chan& ch = state->chans[cid];

        // Validate the response is a real RPC result.
        bool ok = (ch.ctrl->GetErrorCode() == 0)
               && (ch.rsp->ret_code() == 0)
               && (!ch.rsp->order_id().empty());
        int64_t completed = 0;
        if (ok) {
            completed = state->total.fetch_add(1) + 1;
        } else {
            state->errors.fetch_add(1);
        }
        // A global mutex on every completion serialized the client IO loops
        // and measured the recorder rather than RPC capacity.  A deterministic
        // sample still yields thousands of latency observations per run.
        if (ok && (completed & kLatencySampleMask) == 0) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_send).count();
            state->latency.record(static_cast<int64_t>(us));
        }

        if (state->stop.load(std::memory_order_relaxed)) {
            delete this;
            return;
        }

        // Reuse req/rsp/ctrl — Clear() instead of NewMessage (Hical pattern).
        // The request object and its potentially large payload stay allocated;
        // only the field that changes between calls is updated.
        ch.req->set_price(static_cast<int>(state->total.load()));
        ch.rsp->Clear();
        ch.ctrl->Reset();
        ch.ctrl->SetTimeout(ch.timeout_ms);

        auto send_time = std::chrono::steady_clock::now();
        auto* cb = allocCallback();
        cb->init(state, cid, send_time);
        ch.ch->CallMethod(ch.method, ch.ctrl.get(),
                          ch.req.get(), ch.rsp.get(), cb);
        delete this;
    }

    // Override operator delete: return to thread_local pool (Hical pattern).
    static void operator delete(void* p) noexcept {
        if (t_cb_count < kCbPoolMax) {
            auto* slot = reinterpret_cast<CbSlot*>(p);
            slot->next = t_cb_head;
            t_cb_head = slot;
            ++t_cb_count;
        } else {
            ::operator delete(p);
        }
    }
};

// ── Pool impl ─────────────────────────────────────────────────────────
BenchCallback* allocCallback() {
    if (t_cb_head) {
        void* p = t_cb_head;
        t_cb_head = t_cb_head->next;
        --t_cb_count;
        return ::new (p) BenchCallback;
    }
    return new BenchCallback;
}

int main(int argc, char* argv[]) {
    int n_conns     = argc > 1 ? std::atoi(argv[1]) : 100;
    int duration_s  = argc > 2 ? std::atoi(argv[2]) : 10;
    int pipeline    = argc > 3 ? std::atoi(argv[3]) : 1;
    int timeout_ms  = argc > 4 ? std::atoi(argv[4]) : 5000;
    int server_io   = argc > 5 ? std::atoi(argv[5]) : 4;
    int server_workers = argc > 6 ? std::atoi(argv[6]) : 0;
    int payload_bytes = argc > 7 ? std::max(0, std::atoi(argv[7])) : 5;

    int n_inflight = n_conns * pipeline;

    setbuf(stdout, nullptr);
    rocket::Logger::Options opts;
    opts.file_path = "/dev/null";
    // Measure RPC transport/dispatch capacity, not INFO-log formatting.
    // RpcChannel and RpcDispatcher emit multiple INFO records per request.
    opts.level = rocket::LogLevel::Error;
    rocket::Logger::getInstance().start(opts);

    // Set server IO threads via temp config (default 4 from ConfigData).
    if (server_io > 0) {
        std::string yaml = "LOG:\n  file_name: /dev/null\n  level: ERROR\n"
                           "SERVER:\n  port: 12998\n  io_threads: "
                         + std::to_string(server_io) + "\n";
        std::string path = "/tmp/bench_config_" + std::to_string(getpid()) + ".yaml";
        std::ofstream ofs(path);
        ofs << yaml;
        ofs.close();
        rocket::Config::getInstance().reload(path);
    }

    auto addr = std::make_shared<rocket::IPNetAddr>("127.0.0.1", 12998);
    // This benchmark intentionally terminates with _exit().  Keep the
    // process-default SIGINT/SIGTERM behavior so an interrupted warm-up
    // cannot leave a detached embedded server holding port 12998.
    rocket::RpcServer server(
        addr, static_cast<std::size_t>(std::max(server_workers, 1)), false);
    if (server_workers > 0) {
        server.setDefaultExecutionMode(rocket::RpcExecutionMode::WorkerPool);
    }
    server.registerService(std::make_shared<OrderImpl>());
    std::thread svr([&] { server.start(); });
    svr.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    printf("async_bench: connections=%d pipeline=%d inflight=%d duration=%ds "
           "server_io=%d execution=%s workers=%d payload=%dB\n",
           n_conns, pipeline, n_inflight, duration_s, server_io,
           server_workers > 0 ? "worker" : "inline", server_workers,
           payload_bytes);

    // c connections, each shared by pipeline channels (Hical-style pipelining).
    auto pool = std::make_shared<rocket::RpcConnectionPool>(4, n_conns);
    // Fill the pool before callbacks begin recursively issuing requests.
    // Otherwise an early response can re-enter acquire() on a pool IO thread
    // while the pool is still growing and block that loop in connectSync().
    for (int i = 0; i < n_conns; ++i) {
        if (!pool->acquire(addr, timeout_ms)) {
            fprintf(stderr, "failed to warm connection %d/%d\n", i + 1, n_conns);
            return 1;
        }
    }
    auto method = Order::descriptor()->FindMethodByName("makeOrder");

    BenchState state;
    std::string payload(static_cast<std::size_t>(payload_bytes), 'x');
    if (payload_bytes == 5) payload = "async";
    std::vector<std::shared_ptr<rocket::RpcChannel>> connection_channels;
    connection_channels.reserve(n_conns);
    for (int i = 0; i < n_conns; ++i) {
        connection_channels.push_back(
            std::make_shared<rocket::RpcChannel>(addr, pool));
    }

    state.chans.reserve(n_inflight);
    for (int i = 0; i < n_inflight; ++i) {
        // One multi-flight channel per real TCP connection.  Pipeline lanes
        // share that channel and become independent RequestStates instead of
        // multiplying channel objects.
        auto ch = connection_channels[static_cast<std::size_t>(i / pipeline)];
        auto req = rocket::NewMessage<makeOrderRequest>();
        auto rsp = rocket::NewMessage<makeOrderResponse>();
        auto ctrl = rocket::NewRpcController();
        state.chans.push_back(Chan{ch, method, timeout_ms, req, rsp, ctrl});
    }

    // Fire the first request for every in-flight lane.
    for (int cid = 0; cid < n_inflight; ++cid) {
        auto& ch = state.chans[cid];
        ch.req->set_price(cid * 1000);
        ch.req->set_goods(payload);
        ch.ctrl->SetTimeout(timeout_ms);

        auto* cb = allocCallback();
        auto send_time = std::chrono::steady_clock::now();
        cb->init(&state, cid, send_time);
        ch.ch->CallMethod(method, ch.ctrl.get(), ch.req.get(), ch.rsp.get(), cb);
    }

    auto t0 = std::chrono::steady_clock::now();
    int64_t last = 0;
    for (int s = 0; s < duration_s; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int64_t cur = state.total.load();
        printf("  t=%ds  rps=%lld  total=%lld  err=%lld\n",
               s+1, (long long)(cur-last), (long long)cur,
               (long long)state.errors.load());
        fflush(stdout);
        last = cur;
    }

    state.stop.store(true, std::memory_order_release);
    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count()/1000.0;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    printf("\nconnections=%d  inflight=%d  duration=%.1fs  model=async(callback)\n",
           n_conns, n_inflight, elapsed);
    printf("total=%lld  errors=%lld  rps=%.0f\n",
           (long long)state.total.load(), (long long)state.errors.load(),
           state.total.load()/elapsed);
    state.latency.print();
    // Keep alive for profiling
    _exit(0);
}
