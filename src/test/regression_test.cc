#include "order.pb.h"
#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/thread_pool.h"
#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/rpc/coroutine.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/rpc/rpc_server.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include "rocket/net/tcp/tcp_client.h"
#include "rocket/net/timing_wheel.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t reserveUnusedPort();

std::string encodeMessage(const std::shared_ptr<rocket::TinyPBProtocol>& message) {
    rocket::TinyPBCoder coder;
    auto buffer = std::make_shared<rocket::TcpBuffer>();
    std::vector<rocket::AbstractProtocol::s_ptr> messages{message};
    require(coder.encode(messages, buffer), "TinyPB encode failed");
    return std::string(buffer->readableView());
}

void testTinyPBLargeFrameAndPrefix() {
    auto message = std::make_shared<rocket::TinyPBProtocol>();
    message->m_msg_id = "large-message";
    message->m_method_name = "pkg.example.Order.Make";
    message->m_pb_data.assign(4096, 'x');

    const std::string frame = encodeMessage(message);
    require(frame.size() > 512, "large frame did not use heap path");
    require(frame.find_first_not_of('\0') != std::string::npos,
            "heap-encoded frame was zero-filled");

    auto input = std::make_shared<rocket::TcpBuffer>();
    require(input->append("garbage", 7), "failed to append prefix");
    require(input->append(frame), "failed to append frame");

    rocket::TinyPBCoder coder;
    auto decoded = coder.decode(input);
    require(!decoded.fatal && decoded.messages.size() == 1,
            "prefixed TinyPB frame did not decode");
    auto result = std::dynamic_pointer_cast<rocket::TinyPBProtocol>(decoded.messages.front());
    require(result && result->m_pb_data == message->m_pb_data,
            "large TinyPB payload was corrupted");
    require(input->readAble() == 0, "decoder left bytes from garbage prefix");
}

void testTinyPBRejectsMalformedFrames() {
    auto message = std::make_shared<rocket::TinyPBProtocol>();
    message->m_msg_id = "checksum";
    message->m_method_name = "Order.Make";
    message->m_pb_data = "payload";
    const std::string valid = encodeMessage(message);

    {
        std::string corrupted = valid;
        corrupted[corrupted.size() - 6] ^= 0x01;
        auto input = std::make_shared<rocket::TcpBuffer>();
        require(input->append(corrupted), "failed to append corrupted frame");
        rocket::TinyPBCoder coder;
        auto decoded = coder.decode(input);
        require(decoded.fatal && decoded.messages.empty(),
                "checksum mismatch was accepted");
    }

    {
        std::string negative_length = valid;
        const std::uint32_t encoded_negative = htonl(0xffffffffU);
        std::memcpy(negative_length.data() + 5, &encoded_negative,
                    sizeof(encoded_negative));
        auto input = std::make_shared<rocket::TcpBuffer>();
        require(input->append(negative_length), "failed to append malformed frame");
        rocket::TinyPBCoder coder;
        auto decoded = coder.decode(input);
        require(decoded.fatal && decoded.messages.empty(),
                "negative TinyPB field length was accepted");
    }
}

void testBufferOverflowIsExplicit() {
    rocket::TcpBuffer buffer;
    const char byte = 'x';
    require(!buffer.append(&byte, rocket::TcpBuffer::kMaxBufferSize),
            "oversized append did not report failure");
    require(buffer.readAble() == 0, "oversized append partially modified buffer");
}

void testBufferWritesContiguousBytesToFd() {
    int fds[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
            "buffer write socketpair failed");

    rocket::TcpBuffer buffer;
    require(buffer.append("ping", 4), "buffer write append failed");
    int saved_errno = 0;
    require(buffer.writeToFd(fds[0], &saved_errno) == 4,
            "buffer write did not send all bytes");
    require(buffer.empty(), "buffer write did not consume sent bytes");

    char received[4] = {};
    require(::read(fds[1], received, sizeof(received)) == 4,
            "buffer write peer did not receive bytes");
    require(std::string_view(received, sizeof(received)) == "ping",
            "buffer write corrupted bytes");

    ::close(fds[0]);
    ::close(fds[1]);
}

void testLiteralLogFormatting() {
    rocket::Logger::LogEntry entry;
    entry.setArgs("literal log message");
    std::string output;
    entry.runFormat(output);
    require(output == "literal log message", "literal log message was dropped");
}

void testDisabledLogDoesNotEvaluateArguments() {
    auto& logger = rocket::Logger::getInstance();
    logger.stop();
    rocket::Logger::Options options;
    options.file_path = "/dev/null";
    options.level = rocket::LogLevel::Error;
    logger.start(options);

    bool evaluated = false;
    auto expensive_argument = [&] {
        evaluated = true;
        return std::string("should not be evaluated");
    };
    ROCKET_LOG_INFO("disabled argument: {}", expensive_argument());
    require(!evaluated, "disabled log evaluated its arguments");
}

void testTimingWheelKeepsPartialTicks() {
    rocket::TimingWheel wheel;
    std::atomic<bool> fired{false};
    auto timer = rocket::TimerEvent::create(10, false, [&] { fired = true; });
    wheel.addEvent(timer);

    for (int i = 0; i < 50 && !fired.load(); ++i) {
        std::this_thread::sleep_for(1ms);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        wheel.fireExpired(now);
    }
    require(fired.load(), "short polls prevented timing-wheel advancement");
}

void testTimingWheelEagerlyRemovesLongCancelledTimer() {
    rocket::TimingWheel wheel;
    std::atomic<bool> fired{false};
    auto timer = rocket::TimerEvent::create(5000, false, [&] { fired = true; });
    wheel.addEvent(timer);
    require(wheel.pendingCount() == 1, "long timer was not scheduled");

    wheel.cancelEvent(timer);
    require(timer->isCancelled(), "timer cancellation was not published");
    require(wheel.pendingCount() == 0,
            "cancelled long timer remained in the overflow queue");
    require(!fired.load(), "cancelled timer fired");
}

void testThreadPoolRejectsWhenBoundedQueueIsFull() {
    rocket::ThreadPool pool(1, 1);
    std::mutex mutex;
    std::condition_variable cv;
    bool first_started = false;
    bool release_first = false;
    std::atomic<int> completed{0};

    require(pool.tryExecute([&] {
        std::unique_lock<std::mutex> lock(mutex);
        first_started = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_first; });
        completed.fetch_add(1, std::memory_order_relaxed);
    }), "worker pool rejected its first task");

    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, 1s, [&] { return first_started; }),
                "worker pool did not start its first task");
    }

    require(pool.tryExecute(
                [&] { completed.fetch_add(1, std::memory_order_relaxed); }),
            "worker pool rejected a task with queue capacity available");
    require(!pool.tryExecute([] {}),
            "worker pool accepted a task beyond its bounded queue");

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_first = true;
    }
    cv.notify_all();
    pool.stopAndDrain();
    require(completed.load(std::memory_order_relaxed) == 2,
            "worker pool did not drain accepted tasks");
}

class ExecutionRecordingOrder final : public Order {
  public:
    void makeOrder(::google::protobuf::RpcController*,
                   const ::makeOrderRequest* request,
                   ::makeOrderResponse* response,
                   ::google::protobuf::Closure* done) override {
        ran_on_event_loop.store(
            rocket::EventLoop::GetCurrentEventLoop() != nullptr,
            std::memory_order_release);
        called.store(true, std::memory_order_release);
        response->set_ret_code(0);
        response->set_res_info("OK");
        response->set_order_id("thread-" + std::to_string(request->price()));
        if (done) done->Run();
    }

    std::atomic<bool> called{false};
    std::atomic<bool> ran_on_event_loop{false};
};

void testRpcExecutionMode(rocket::RpcExecutionMode default_mode,
                          bool override_method_inline,
                          bool expected_event_loop) {
    const auto port = reserveUnusedPort();
    auto address = std::make_shared<rocket::IPNetAddr>("127.0.0.1", port);
    auto service = std::make_shared<ExecutionRecordingOrder>();

    rocket::RpcServer server(address, 2, false, 8);
    require(server.setDefaultExecutionMode(default_mode),
            "failed to configure default RPC execution mode");
    if (override_method_inline) {
        require(server.setMethodExecutionMode(
                    "Order.makeOrder", rocket::RpcExecutionMode::Inline),
                "failed to configure method RPC execution mode");
    }
    server.registerService(service);

    std::thread server_thread([&] { server.start(); });
    std::this_thread::sleep_for(100ms);
    require(!server.setDefaultExecutionMode(rocket::RpcExecutionMode::Inline),
            "RPC execution mode changed after server start");

    auto pool = std::make_shared<rocket::RpcConnectionPool>(1, 1);
    auto channel = std::make_shared<rocket::RpcChannel>(address, pool);
    makeOrderRequest request;
    request.set_price(42);
    makeOrderResponse response;
    rocket::RpcController controller;
    const auto* method = Order::descriptor()->FindMethodByName("makeOrder");

    const int call_result = channel->CallMethodBlocking(
        method, &controller, &request, &response, 2000);
    pool->shutdown();
    server.stop();
    server_thread.join();

    require(call_result == 0, "RPC execution-mode test call failed");
    require(service->called.load(std::memory_order_acquire),
            "configured RPC service method was not called");
    require(service->ran_on_event_loop.load(std::memory_order_acquire) ==
                expected_event_loop,
            "RPC service method ran on the wrong executor");
}

void testRpcExecutionModes() {
    testRpcExecutionMode(rocket::RpcExecutionMode::WorkerPool, false, false);
    testRpcExecutionMode(rocket::RpcExecutionMode::WorkerPool, true, true);
}

class BlockingOrder final : public Order {
  public:
    void makeOrder(::google::protobuf::RpcController*,
                   const ::makeOrderRequest* request,
                   ::makeOrderResponse* response,
                   ::google::protobuf::Closure* done) override {
        if (request->price() == 1) {
            std::unique_lock<std::mutex> lock(mutex);
            first_started = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_first; });
        }
        response->set_ret_code(0);
        response->set_res_info("OK");
        response->set_order_id("overload-" + std::to_string(request->price()));
        if (done) done->Run();
    }

    bool waitUntilFirstStarts() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 1s, [&] { return first_started; });
    }

    void releaseFirst() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_first = true;
        }
        cv.notify_all();
    }

  private:
    std::mutex mutex;
    std::condition_variable cv;
    bool first_started{false};
    bool release_first{false};
};

void testRpcWorkerQueueOverloadIsExplicit() {
    const auto port = reserveUnusedPort();
    auto address = std::make_shared<rocket::IPNetAddr>("127.0.0.1", port);
    auto service = std::make_shared<BlockingOrder>();

    rocket::RpcServer server(address, 1, false, 1);
    require(server.setDefaultExecutionMode(rocket::RpcExecutionMode::WorkerPool),
            "failed to enable worker execution for overload test");
    server.registerService(service);
    std::thread server_thread([&] { server.start(); });
    std::this_thread::sleep_for(100ms);

    auto pool = std::make_shared<rocket::RpcConnectionPool>(1, 1);
    const auto* method = Order::descriptor()->FindMethodByName("makeOrder");
    std::atomic<int> first_result{-999};
    std::atomic<int> second_result{-999};

    auto invoke = [&](int price) {
        auto channel = std::make_shared<rocket::RpcChannel>(address, pool);
        makeOrderRequest request;
        request.set_price(price);
        makeOrderResponse response;
        rocket::RpcController controller;
        return channel->CallMethodBlocking(
            method, &controller, &request, &response, 3000);
    };

    std::thread first([&] { first_result.store(invoke(1)); });
    const bool first_started = service->waitUntilFirstStarts();

    std::thread second;
    bool second_queued = false;
    int overloaded_result = -999;
    if (first_started) {
        second = std::thread([&] { second_result.store(invoke(2)); });
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (server.pendingWorkerTasks() == 1) {
                second_queued = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        if (second_queued) overloaded_result = invoke(3);
    }

    service->releaseFirst();
    first.join();
    if (second.joinable()) second.join();
    pool->shutdown();
    server.stop();
    server_thread.join();

    require(first_started, "blocking worker task did not start");
    require(second_queued, "second worker task was not queued");
    require(overloaded_result == rocket::error::kRpcServerOverloaded,
            "full server worker queue did not return overload");
    require(first_result.load() == 0 && second_result.load() == 0,
            "accepted worker tasks did not complete");
}

struct DelayedResume {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) const {
        std::thread([handle] {
            std::this_thread::sleep_for(20ms);
            handle.resume();
        }).detach();
    }
    void await_resume() const noexcept {}
};

rocket::Task<int> delayedTask() {
    co_await DelayedResume{};
    co_return 42;
}

void testTaskRunWaitsForCompletion() {
    auto task = delayedTask();
    const auto start = std::chrono::steady_clock::now();
    require(task.run() == 42, "Task::run returned the wrong result");
    require(std::chrono::steady_clock::now() - start >= 10ms,
            "Task::run returned before the coroutine resumed");
    require(task.done(), "Task was not complete after run()");
}

std::uint16_t reserveUnusedPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "socket failed");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "bind failed");

    socklen_t length = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "getsockname failed");
    const auto port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

void testConnectFailureAndStandardDone() {
    const auto port = reserveUnusedPort();
    auto address = std::make_shared<rocket::IPNetAddr>("127.0.0.1", port);
    auto channel = std::make_shared<rocket::RpcChannel>(
        address, rocket::RpcConnectionPool::s_ptr{});

    makeOrderRequest request;
    makeOrderResponse response;
    rocket::RpcController controller;
    controller.SetTimeout(500);

    struct DoneState {
        std::mutex mutex;
        std::condition_variable cv;
        bool called{false};
    } state;
    struct DoneClosure final : google::protobuf::Closure {
        DoneState& state;
        explicit DoneClosure(DoneState& value) : state(value) {}
        void Run() override {
            {
                std::lock_guard<std::mutex> lk(state.mutex);
                state.called = true;
            }
            state.cv.notify_one();
        }
    } done(state);

    makeOrderResponse second_response;
    rocket::RpcController second_controller;
    second_controller.SetTimeout(500);
    DoneState second_state;
    DoneClosure second_done(second_state);

    Order_Stub stub(channel.get());
    stub.makeOrder(&controller, &request, &response, &done);
    // Start a second request before the first one completes.  A shared channel
    // must create an independent RequestState instead of rejecting it as
    // "already in flight".
    stub.makeOrder(&second_controller, &request, &second_response, &second_done);

    std::unique_lock<std::mutex> lk(state.mutex);
    require(state.cv.wait_for(lk, 2s, [&] { return state.called; }),
            "protobuf done callback was not invoked without Init()");
    require(controller.GetErrorCode() != 0,
            "refused non-blocking connect was reported as successful");
    require(controller.GetErrorCode() != rocket::error::kRpcChannelInit,
            "shared channel rejected a concurrent RPC");
    const std::string first_msg_id = controller.GetMsgId();
    require(!first_msg_id.empty(), "first RPC did not receive a message ID");
    lk.unlock();

    std::unique_lock<std::mutex> second_lk(second_state.mutex);
    require(second_state.cv.wait_for(
                second_lk, 2s, [&] { return second_state.called; }),
            "second protobuf done callback was not invoked");
    require(second_controller.GetErrorCode() != 0,
            "second refused connect was reported as successful");
    require(second_controller.GetErrorCode() != rocket::error::kRpcChannelInit,
            "second concurrent RPC was rejected by the channel");
    require(!second_controller.GetMsgId().empty(),
            "second RPC did not receive a message ID");
    require(second_controller.GetMsgId() != first_msg_id,
            "concurrent RPCs reused the thread runtime message ID");
}

void testConnectionPoolFillsConfiguredSize() {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listen_fd >= 0, "listen socket failed");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "pool test bind failed");
    require(::listen(listen_fd, 8) == 0, "pool test listen failed");
    const int listen_flags = ::fcntl(listen_fd, F_GETFL, 0);
    require(listen_flags >= 0 &&
                ::fcntl(listen_fd, F_SETFL, listen_flags | O_NONBLOCK) == 0,
            "failed to make listen socket non-blocking");

    socklen_t length = sizeof(address);
    require(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "pool test getsockname failed");
    const auto port = ntohs(address.sin_port);

    std::atomic<bool> stop_server{false};
    std::thread server([&] {
        std::vector<int> accepted;
        while (accepted.size() < 3 &&
               !stop_server.load(std::memory_order_acquire)) {
            const int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd >= 0) {
                accepted.push_back(fd);
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        while (!stop_server.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        for (int fd : accepted) ::close(fd);
        ::close(listen_fd);
    });

    std::exception_ptr failure;
    try {
        rocket::RpcConnectionPool pool(1, 3);
        auto peer = std::make_shared<rocket::IPNetAddr>("127.0.0.1", port);
        auto first = pool.acquire(peer, 1000);
        auto second = pool.acquire(peer, 1000);
        auto third = pool.acquire(peer, 1000);
        require(first && second && third, "connection pool failed to connect");
        require(first != second && first != third && second != third,
                "connection pool did not create the configured slots");

        const std::vector<rocket::TcpClient::s_ptr> slots{first, second, third};
        for (std::size_t i = 0; i < 300; ++i) {
            require(pool.acquire(peer, 1000) == slots[i % slots.size()],
                    "full connection pool did not use stable round-robin slots");
        }
        pool.shutdown();
    } catch (...) {
        failure = std::current_exception();
    }

    stop_server.store(true, std::memory_order_release);
    server.join();
    if (failure) std::rethrow_exception(failure);
}

} // namespace

int main() {
    try {
        testTinyPBLargeFrameAndPrefix();
        testTinyPBRejectsMalformedFrames();
        testBufferOverflowIsExplicit();
        testBufferWritesContiguousBytesToFd();
        testLiteralLogFormatting();
        testDisabledLogDoesNotEvaluateArguments();
        testTimingWheelKeepsPartialTicks();
        testTimingWheelEagerlyRemovesLongCancelledTimer();
        testThreadPoolRejectsWhenBoundedQueueIsFull();
        testRpcExecutionModes();
        testRpcWorkerQueueOverloadIsExplicit();
        testTaskRunWaitsForCompletion();
        testConnectFailureAndStandardDone();
        testConnectionPoolFillsConfiguredSize();
        std::cout << "regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "regression test failed: " << error.what() << '\n';
        return 1;
    }
}
