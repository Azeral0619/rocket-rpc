#include "order.pb.h"
#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/thread_pool.h"
#include "rocket/net/coder/string_coder.h"
#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/rpc/coroutine.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_client.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/rpc/rpc_server.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_acceptor.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include "rocket/net/tcp/tcp_client.h"
#include "rocket/net/tcp/tcp_connection.h"
#include "rocket/net/timing_wheel.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
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
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
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
    message->m_msg_id = 1001;
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

void testTinyPBNumericMessageIdFormat() {
    constexpr rocket::MessageId kMessageId = 0x0102030405060708ULL;
    auto message = std::make_shared<rocket::TinyPBProtocol>();
    message->m_msg_id = kMessageId;
    message->m_method_name = "Order.Make";
    message->m_pb_data = "payload";

    const std::string frame = encodeMessage(message);
    require(frame.size() ==
                rocket::TinyPBProtocol::HEADER_SIZE +
                    message->m_method_name.size() + message->m_pb_data.size(),
            "numeric message ID frame retained variable-length ID overhead");
    constexpr unsigned char kExpectedId[] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    constexpr std::size_t kMessageIdOffset =
        1 + sizeof(std::int32_t);
    require(frame.size() >=
                kMessageIdOffset + sizeof(kExpectedId),
            "numeric message ID frame was too short");
    require(std::memcmp(frame.data() + kMessageIdOffset, kExpectedId,
                        sizeof(kExpectedId)) == 0,
            "numeric message ID was not encoded in network byte order");

    auto input = std::make_shared<rocket::TcpBuffer>();
    require(input->append(frame), "failed to append numeric message ID frame");
    rocket::TinyPBCoder coder;
    auto decoded = coder.decode(input);
    require(!decoded.fatal && decoded.messages.size() == 1,
            "numeric message ID frame did not decode");
    require(decoded.messages.front()->m_msg_id == kMessageId,
            "numeric message ID changed during roundtrip");

    auto generated = std::make_shared<rocket::TinyPBProtocol>();
    generated->m_method_name = "Order.Make";
    const std::string generated_frame = encodeMessage(generated);
    require(!generated_frame.empty() &&
                generated->m_msg_id != rocket::kInvalidMessageId,
            "encoder did not assign a non-zero numeric message ID");
}

void testTinyPBDirectAndBorrowedProtobufPayload() {
    makeOrderRequest request;
    request.set_price(2026);
    request.set_goods("direct-payload");
    const std::string expected = request.SerializeAsString();

    auto message = std::make_shared<rocket::TinyPBProtocol>();
    message->m_msg_id = 2001;
    message->m_method_name = "Order.makeOrder";
    message->setProtobufMessage(&request);

    rocket::TinyPBCoder encoder;
    rocket::TcpBuffer encoded;
    std::vector<rocket::AbstractProtocol::s_ptr> messages{message};
    require(encoder.encode(messages, encoded),
            "direct protobuf TinyPB encode failed");
    require(message->m_pb_data.empty(),
            "direct protobuf encode populated the intermediate string");

    rocket::TcpBuffer input;
    require(input.append(encoded.readableView()),
            "failed to append direct protobuf frame");
    rocket::TinyPBCoder decoder(
        rocket::TinyPBCoder::PayloadMode::Borrowed);
    std::vector<rocket::AbstractProtocol::s_ptr> decoded;
    require(decoder.decode(input, decoded) && decoded.size() == 1,
            "borrowed protobuf TinyPB decode failed");

    auto result = std::dynamic_pointer_cast<rocket::TinyPBProtocol>(
        decoded.front());
    require(result && result->m_pb_data.empty() &&
                result->pbDataView() == expected,
            "borrowed protobuf payload did not reference the encoded bytes");

    makeOrderRequest parsed;
    const auto payload = result->pbDataView();
    require(parsed.ParseFromArray(payload.data(),
                                  static_cast<int>(payload.size())) &&
                parsed.price() == request.price() &&
                parsed.goods() == request.goods(),
            "borrowed protobuf payload could not be parsed directly");
}

void testTinyPBOptionalChecksum() {
    auto message = std::make_shared<rocket::TinyPBProtocol>();
    message->m_msg_id = 2002;
    message->m_method_name = "Order.Make";
    message->m_pb_data = "payload";

    rocket::TinyPBCoder coder(
        rocket::TinyPBCoder::PayloadMode::Owned,
        rocket::TinyPBCoder::ChecksumPolicy::None);
    rocket::TcpBuffer encoded;
    std::vector<rocket::AbstractProtocol::s_ptr> messages{message};
    require(coder.encode(messages, encoded),
            "checksum-free TinyPB encode failed");

    std::string frame(encoded.readableView());
    require(frame.size() > sizeof(std::uint32_t) + 1,
            "checksum-free TinyPB frame was too short");
    std::uint32_t reserved_checksum = 1;
    std::memcpy(&reserved_checksum,
                frame.data() + frame.size() - 1 - sizeof(reserved_checksum),
                sizeof(reserved_checksum));
    require(reserved_checksum == 0,
            "disabled checksum did not leave the reserved field zeroed");

    // A disabled decoder must not scan or validate the reserved checksum
    // field. Keep the frame structure intact and replace only that field.
    frame[frame.size() - 2] ^= static_cast<char>(0xff);
    auto input = std::make_shared<rocket::TcpBuffer>();
    require(input->append(frame), "failed to append checksum-free frame");
    auto decoded = coder.decode(input);
    require(!decoded.fatal && decoded.messages.size() == 1,
            "disabled checksum policy still rejected the checksum field");
}

void testTinyPBRejectsMalformedFrames() {
    auto message = std::make_shared<rocket::TinyPBProtocol>();
    message->m_msg_id = 1002;
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
        constexpr std::size_t kMethodLengthOffset =
            1 + sizeof(std::int32_t) + sizeof(rocket::MessageId);
        std::memcpy(negative_length.data() + kMethodLengthOffset,
                    &encoded_negative,
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

    require(::write(fds[1], "pong", 4) == 4,
            "buffer direct-read peer write failed");
    rocket::TcpBuffer input;
    require(input.readFromFd(fds[0], &saved_errno, false) == 4,
            "buffer direct-read failed");
    require(input.retrieveAll() == "pong", "buffer direct-read corrupted bytes");

    ::close(fds[0]);
    ::close(fds[1]);
}

void testBoundedConnectionWriteQueueHandlesConcurrentProducers() {
    int fds[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
            "write-queue socketpair failed");

    constexpr int kProducerCount = 4;
    constexpr int kMessagesPerProducer = 256;
    constexpr std::size_t kMessageSize = 32;
    constexpr std::size_t kExpectedBytes =
        kProducerCount * kMessagesPerProducer * kMessageSize;

    std::mutex mutex;
    std::condition_variable cv;
    rocket::EventLoop* loop = nullptr;
    rocket::TcpConnection::s_ptr connection;
    bool ready = false;

    std::thread io_thread([&] {
        rocket::EventLoop event_loop;
        auto address =
            std::make_shared<rocket::IPNetAddr>("127.0.0.1", 0);
        auto owned_connection = std::make_shared<rocket::TcpConnection>(
            &event_loop, fds[0], address, address,
            std::make_unique<rocket::StringCoder>(),
            rocket::TcpConnectionType::Server);
        event_loop.queueInLoop([&] {
            owned_connection->connectEstablished();
            {
                std::lock_guard<std::mutex> lock(mutex);
                loop = &event_loop;
                connection = owned_connection;
                ready = true;
            }
            cv.notify_one();
        });

        event_loop.loop();
        owned_connection->connectDestroyed();
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, 1s, [&] { return ready; }),
                "write-queue EventLoop did not start");
    }

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([connection, producer] {
            const char marker = static_cast<char>('A' + producer);
            for (int i = 0; i < kMessagesPerProducer; ++i) {
                auto message = std::make_shared<rocket::StringProtocol>();
                message->info.assign(kMessageSize, marker);
                connection->send(std::move(message));
            }
        });
    }
    for (auto& producer : producers) producer.join();

    std::string received;
    received.reserve(kExpectedBytes);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (received.size() < kExpectedBytes &&
           std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{fds[1], POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, 20);
        if (poll_result < 0 && errno == EINTR) continue;
        require(poll_result >= 0, "write-queue peer poll failed");
        if (poll_result == 0 || (descriptor.revents & POLLIN) == 0) continue;

        char buffer[4096];
        const ssize_t bytes = ::read(fds[1], buffer, sizeof(buffer));
        require(bytes > 0, "write-queue peer read failed");
        received.append(buffer, static_cast<std::size_t>(bytes));
    }

    require(received.size() == kExpectedBytes,
            "bounded write queue lost cross-thread messages");
    for (std::size_t offset = 0; offset < received.size();
         offset += kMessageSize) {
        const char marker = received[offset];
        require(marker >= 'A' && marker < 'A' + kProducerCount,
                "bounded write queue corrupted a producer marker");
        for (std::size_t i = 1; i < kMessageSize; ++i) {
            require(received[offset + i] == marker,
                    "bounded write queue interleaved encoded messages");
        }
    }

    auto oversized = std::make_shared<rocket::StringProtocol>();
    oversized->info.resize(4ULL * 1024 * 1024 + 1, 'x');
    connection->send(std::move(oversized));
    const auto close_deadline = std::chrono::steady_clock::now() + 1s;
    while (connection->getState() != rocket::TcpState::Closed &&
           std::chrono::steady_clock::now() < close_deadline) {
        std::this_thread::yield();
    }
    require(connection->getState() == rocket::TcpState::Closed,
            "oversized cross-thread write did not close the connection");

    loop->stop();
    io_thread.join();
    connection.reset();
    ::close(fds[1]);
}

void testAcceptorDrainsWithoutBlocking() {
    auto address = std::make_shared<rocket::IPNetAddr>("127.0.0.1", 0);
    rocket::TcpAcceptor acceptor(address);
    require(acceptor.isListening(), "acceptor failed to listen");

    sockaddr_in bound_address{};
    socklen_t bound_length = sizeof(bound_address);
    require(::getsockname(
                acceptor.getListenFd(),
                reinterpret_cast<sockaddr*>(&bound_address),
                &bound_length) == 0,
            "failed to read acceptor address");

    constexpr int kConnections = 16;
    std::vector<int> clients;
    clients.reserve(kConnections);
    for (int i = 0; i < kConnections; ++i) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "failed to create acceptor test client");
        require(::connect(fd, reinterpret_cast<sockaddr*>(&bound_address),
                          sizeof(bound_address)) == 0,
                "failed to connect acceptor test client");
        clients.push_back(fd);
    }

    std::vector<int> accepted;
    for (;;) {
        auto result = acceptor.accept();
        if (!result.isValid()) {
            require(result.wouldBlock(),
                    "acceptor drain ended with an unexpected error");
            break;
        }
        const int status_flags = ::fcntl(result.client_fd, F_GETFL, 0);
        const int descriptor_flags = ::fcntl(result.client_fd, F_GETFD, 0);
        require(status_flags >= 0 && (status_flags & O_NONBLOCK) != 0,
                "accepted socket was blocking");
        require(descriptor_flags >= 0 &&
                    (descriptor_flags & FD_CLOEXEC) != 0,
                "accepted socket could leak across exec");
        accepted.push_back(result.client_fd);
    }

    require(accepted.size() == clients.size(),
            "acceptor did not drain every queued connection");
    for (int fd : accepted) ::close(fd);
    for (int fd : clients) ::close(fd);
}

void testLiteralLogFormatting() {
    rocket::Logger::LogEntry entry;
    entry.setArgs("literal log message");
    std::string output;
    entry.runFormat(output);
    require(output == "literal log message", "literal log message was dropped");
}

void testTypedLogFormatting() {
    rocket::Logger::LogEntry entry;
    const std::string large(96, 'x');
    static constexpr auto metadata = rocket::Logger::makeLogMetadata<
        int, unsigned, std::int64_t, std::uint64_t, unsigned, double, bool,
        char, const std::string&, std::string_view>(
        "i={} u={} i64={} u64={} hex={:02x} d={:.2f} b={} c={} s={} sv={}",
        rocket::LogLevel::Info);
    entry.setArgs(
        &metadata,
        -7, 9U, std::int64_t{-11}, std::uint64_t{13}, 0x0aU, 3.25,
        true, 'z', large, std::string_view{"tail"});
    std::string output;
    entry.runFormat(output);
    const std::string expected =
        "i=-7 u=9 i64=-11 u64=13 hex=0a d=3.25 b=true c=z s=" +
        large + " sv=tail";
    require(output == expected, "typed async log codec changed formatting");

    output.clear();
    entry.setArgs("reuse={}", 42);
    entry.runFormat(output);
    require(output == "reuse=42", "heap-backed log entry was not reusable");
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

void testTimingWheelEagerlyRemovesShortCancelledTimer() {
    rocket::TimingWheel wheel;
    auto retained = std::make_shared<int>(1);
    std::weak_ptr<int> weak_retained = retained;
    auto timer =
        rocket::TimerEvent::create(1000, false, [retained] { (void)retained; });
    retained.reset();

    wheel.addEvent(timer);
    require(wheel.pendingCount() == 1, "short timer was not scheduled");
    require(!weak_retained.expired(), "timer callback state was not retained");

    wheel.cancelEvent(timer);
    require(wheel.pendingCount() == 0,
            "cancelled short timer remained in the timing-wheel slot");
    timer.reset();
    require(weak_retained.expired(),
            "cancelled short timer retained its callback state");
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

class AsyncCompletionOrder final : public Order {
  public:
    void makeOrder(::google::protobuf::RpcController*,
                   const ::makeOrderRequest* request,
                   ::makeOrderResponse* response,
                   ::google::protobuf::Closure* done) override {
        const int price = request->price();
        std::thread([price, response, done] {
            std::this_thread::sleep_for(20ms);
            response->set_ret_code(0);
            response->set_res_info("OK");
            response->set_order_id("async-" + std::to_string(price));
            if (done) done->Run();
        }).detach();
    }
};

void testAsyncRpcServiceRetainsCallState() {
    const auto port = reserveUnusedPort();
    auto address = std::make_shared<rocket::IPNetAddr>("127.0.0.1", port);

    rocket::RpcServer server(address, 1, false);
    server.registerService(std::make_shared<AsyncCompletionOrder>());
    std::thread server_thread([&] { server.start(); });
    std::this_thread::sleep_for(100ms);

    auto pool = std::make_shared<rocket::RpcConnectionPool>(1, 1);
    auto channel = std::make_shared<rocket::RpcChannel>(address, pool);
    makeOrderRequest request;
    request.set_price(73);
    makeOrderResponse response;
    rocket::RpcController controller;
    const auto* method = Order::descriptor()->FindMethodByName("makeOrder");

    const int result = channel->CallMethodBlocking(
        method, &controller, &request, &response, 2000);
    pool->shutdown();
    server.stop();
    server_thread.join();

    require(result == 0, "asynchronous RPC service call failed");
    require(response.order_id() == "async-73",
            "asynchronous RPC service lost its retained call state");
}

rocket::Task<std::string> callOrderWithTypedClient(
    rocket::Client<Order_Stub> client, int price) {
    makeOrderRequest request;
    request.set_price(price);
    request.set_goods("coroutine");

    auto result =
        co_await client.call<&Order_Stub::makeOrder>(
            request, rocket::CallOptions{.timeout = 2s});
    if (!result) {
        co_return std::string("error:") +
                  std::to_string(result.status().code());
    }
    co_return std::move(result).value().order_id();
}

void testTypedClientCallModes() {
    const auto port = reserveUnusedPort();
    auto address =
        std::make_shared<rocket::IPNetAddr>("127.0.0.1", port);
    auto service = std::make_shared<ExecutionRecordingOrder>();

    rocket::RpcServer server(address, 2, false);
    server.registerService(service);
    std::thread server_thread([&] { server.start(); });
    std::this_thread::sleep_for(100ms);

    auto pool = std::make_shared<rocket::RpcConnectionPool>(1, 1);
    auto channel = std::make_shared<rocket::RpcChannel>(address, pool);
    auto client = rocket::MakeClient<Order_Stub>(channel);

    auto invalid_channel = std::make_shared<rocket::RpcChannel>(
        rocket::NetAddr::s_ptr{}, pool);
    auto invalid_client =
        rocket::MakeClient<Order_Stub>(std::move(invalid_channel));
    makeOrderRequest invalid_request;
    auto invalid_result =
        invalid_client.callBlocking<&Order_Stub::makeOrder>(
            invalid_request,
            rocket::CallOptions{.timeout = 2s});
    require(!invalid_result,
            "typed client accepted a missing peer address");
    require(invalid_result.status().code() == rocket::error::kRpcPeerAddr,
            "typed client lost a synchronous channel error");

    makeOrderRequest blocking_request;
    blocking_request.set_price(101);
    blocking_request.set_goods("blocking");
    auto blocking_result =
        client.callBlocking<&Order_Stub::makeOrder>(
            blocking_request,
            rocket::CallOptions{.timeout = 2s});
    require(blocking_result.ok(), "typed blocking RPC failed");
    require(blocking_result.value().order_id() == "thread-101",
            "typed blocking RPC returned the wrong response");

    struct AsyncState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
        bool ok{false};
        std::string order_id;
    };
    auto async_state = std::make_shared<AsyncState>();
    {
        makeOrderRequest async_request;
        async_request.set_price(202);
        async_request.set_goods("callback");
        client.callAsync<&Order_Stub::makeOrder>(
            async_request, rocket::CallOptions{.timeout = 2s},
            [async_state](rocket::RpcResult<makeOrderResponse> result) {
                {
                    std::lock_guard<std::mutex> lock(async_state->mutex);
                    async_state->ok = result.ok();
                    if (result) {
                        async_state->order_id =
                            std::move(result).value().order_id();
                    }
                    async_state->done = true;
                }
                async_state->cv.notify_one();
            });
    }
    {
        std::unique_lock<std::mutex> lock(async_state->mutex);
        require(async_state->cv.wait_for(
                    lock, 2s, [&] { return async_state->done; }),
                "typed callback RPC did not complete");
        require(async_state->ok, "typed callback RPC failed");
        require(async_state->order_id == "thread-202",
                "typed callback RPC returned the wrong response");
    }

    auto coroutine = callOrderWithTypedClient(client, 303);
    require(coroutine.run() == "thread-303",
            "typed coroutine RPC returned the wrong response");

    pool->shutdown();
    server.stop();
    server_thread.join();
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
    const rocket::MessageId first_msg_id = controller.GetMsgId();
    require(first_msg_id != rocket::kInvalidMessageId,
            "first RPC did not receive a message ID");
    lk.unlock();

    std::unique_lock<std::mutex> second_lk(second_state.mutex);
    require(second_state.cv.wait_for(
                second_lk, 2s, [&] { return second_state.called; }),
            "second protobuf done callback was not invoked");
    require(second_controller.GetErrorCode() != 0,
            "second refused connect was reported as successful");
    require(second_controller.GetErrorCode() != rocket::error::kRpcChannelInit,
            "second concurrent RPC was rejected by the channel");
    require(second_controller.GetMsgId() != rocket::kInvalidMessageId,
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
        testTinyPBNumericMessageIdFormat();
        testTinyPBDirectAndBorrowedProtobufPayload();
        testTinyPBOptionalChecksum();
        testTinyPBRejectsMalformedFrames();
        testBufferOverflowIsExplicit();
        testBufferWritesContiguousBytesToFd();
        testBoundedConnectionWriteQueueHandlesConcurrentProducers();
        testAcceptorDrainsWithoutBlocking();
        testLiteralLogFormatting();
        testTypedLogFormatting();
        testDisabledLogDoesNotEvaluateArguments();
        testTimingWheelKeepsPartialTicks();
        testTimingWheelEagerlyRemovesLongCancelledTimer();
        testTimingWheelEagerlyRemovesShortCancelledTimer();
        testThreadPoolRejectsWhenBoundedQueueIsFull();
        testRpcExecutionModes();
        testAsyncRpcServiceRetainsCallState();
        testTypedClientCallModes();
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
