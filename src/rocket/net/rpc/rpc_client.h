#pragma once

#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <google/protobuf/service.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace rocket {

// Options that belong to one RPC attempt.  Connection and load-balancing
// options belong to RpcChannel/RpcConnectionPool instead.
struct CallOptions {
    std::chrono::milliseconds timeout{3000};
};

class RpcStatus {
  public:
    RpcStatus() = default;
    RpcStatus(int code, std::string message)
        : m_code(code), m_message(std::move(message)) {}

    [[nodiscard]] bool ok() const noexcept { return m_code == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] int code() const noexcept { return m_code; }
    [[nodiscard]] const std::string& message() const noexcept {
        return m_message;
    }

  private:
    int m_code{0};
    std::string m_message;
};

template <typename Response>
class RpcResult {
  public:
    static RpcResult success(Response response) {
        return RpcResult(RpcStatus{}, std::move(response));
    }

    static RpcResult failure(RpcStatus status) {
        return RpcResult(std::move(status), std::nullopt);
    }

    RpcResult(RpcResult&&) = default;
    RpcResult& operator=(RpcResult&&) = default;
    RpcResult(const RpcResult&) = delete;
    RpcResult& operator=(const RpcResult&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        return m_status.ok() && m_value.has_value();
    }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const RpcStatus& status() const noexcept { return m_status; }

    Response& value() & { return m_value.value(); }
    const Response& value() const& { return m_value.value(); }
    Response&& value() && { return std::move(m_value).value(); }

  private:
    RpcResult(RpcStatus status, std::optional<Response> value)
        : m_status(std::move(status)), m_value(std::move(value)) {}

    RpcStatus m_status;
    std::optional<Response> m_value;
};

template <typename Response>
using RpcCallback = std::function<void(RpcResult<Response>)>;

namespace detail {

// Traits for the unary method signature emitted by protobuf when
// cc_generic_services is enabled.
template <typename Method>
struct RpcMethodTraits;

template <typename Service, typename Request, typename Response>
struct RpcMethodTraits<void (Service::*)(
    google::protobuf::RpcController*, const Request*, Response*,
    google::protobuf::Closure*)> {
    using service_type = Service;
    using request_type = Request;
    using response_type = Response;
};

inline int timeoutMilliseconds(std::chrono::milliseconds timeout) noexcept {
    const auto count = timeout.count();
    if (count <= 0) return 1;
    if (count > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(count);
}

template <typename Response>
class RpcCallState final : public google::protobuf::Closure {
  public:
    RpcController controller;
    Response response;

    // Keep this completion object alive even if the returned RpcCall is
    // discarded before the response arrives.  Run() breaks the cycle.
    void arm(std::shared_ptr<RpcCallState> self) {
        m_completion_owner = std::move(self);
    }

    void Run() override {
        auto completion_owner = std::move(m_completion_owner);
        complete();
    }

    void complete() {
        RpcCallback<Response> callback;
        std::coroutine_handle<> continuation;
        std::optional<RpcResult<Response>> callback_result;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_completed) return;

            const int error_code = controller.GetErrorCode();
            if (error_code != 0 || controller.Failed()) {
                m_status = RpcStatus(
                    error_code != 0 ? error_code : -1,
                    controller.GetErrorInfo());
            } else {
                m_status = RpcStatus{};
            }
            m_completed = true;

            if (m_callback) {
                callback = std::move(m_callback);
                callback_result.emplace(takeResultLocked());
            } else {
                continuation = m_continuation;
                m_continuation = nullptr;
            }
        }

        m_cv.notify_all();
        if (callback) {
            invokeCallback(callback, std::move(*callback_result));
        } else if (continuation) {
            continuation.resume();
        }
    }

    [[nodiscard]] bool isCompleted() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_completed;
    }

    // Returning false from await_suspend continues the coroutine without
    // suspending when completion won the registration race.
    bool suspend(std::coroutine_handle<> continuation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed) return false;
        m_continuation = continuation;
        return true;
    }

    RpcResult<Response> awaitResult() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return takeResultLocked();
    }

    RpcResult<Response> wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_completed; });
        return takeResultLocked();
    }

    void setCallback(RpcCallback<Response> callback) {
        std::optional<RpcResult<Response>> completed_result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_completed) {
                completed_result.emplace(takeResultLocked());
            } else {
                m_callback = std::move(callback);
                return;
            }
        }
        invokeCallback(callback, std::move(*completed_result));
    }

  private:
    RpcResult<Response> takeResultLocked() {
        if (m_consumed) {
            return RpcResult<Response>::failure(
                RpcStatus{-1, "RPC result already consumed"});
        }
        m_consumed = true;
        if (!m_status.ok()) {
            return RpcResult<Response>::failure(m_status);
        }
        return RpcResult<Response>::success(std::move(response));
    }

    static void invokeCallback(RpcCallback<Response>& callback,
                               RpcResult<Response> result) noexcept {
        try {
            callback(std::move(result));
        } catch (const std::exception& error) {
            ROCKET_LOG_ERROR("RPC callback threw: {}", error.what());
        } catch (...) {
            ROCKET_LOG_ERROR("RPC callback threw an unknown exception");
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_completed{false};
    bool m_consumed{false};
    RpcStatus m_status;
    std::coroutine_handle<> m_continuation;
    RpcCallback<Response> m_callback;
    std::shared_ptr<RpcCallState> m_completion_owner;
};

template <typename Stub>
struct ClientState {
    explicit ClientState(std::shared_ptr<RpcChannel> value)
        : channel(std::move(value)), stub(channel.get()) {}

    std::shared_ptr<RpcChannel> channel;
    Stub stub;
};

} // namespace detail

template <typename Response>
class RpcCall {
  public:
    RpcCall(RpcCall&&) noexcept = default;
    RpcCall& operator=(RpcCall&&) noexcept = default;
    RpcCall(const RpcCall&) = delete;
    RpcCall& operator=(const RpcCall&) = delete;

    class Awaiter {
      public:
        explicit Awaiter(std::shared_ptr<detail::RpcCallState<Response>> state)
            : m_state(std::move(state)) {}

        [[nodiscard]] bool await_ready() const {
            return m_state->isCompleted();
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            return m_state->suspend(continuation);
        }
        RpcResult<Response> await_resume() {
            return m_state->awaitResult();
        }

      private:
        std::shared_ptr<detail::RpcCallState<Response>> m_state;
    };

    Awaiter operator co_await() && {
        return Awaiter(std::move(m_state));
    }

    RpcResult<Response> get() && {
        return m_state->wait();
    }

    template <typename Callback>
    void then(Callback&& callback) && {
        m_state->setCallback(
            RpcCallback<Response>(std::forward<Callback>(callback)));
        m_state.reset();
    }

  private:
    template <typename Stub>
    friend class Client;

    explicit RpcCall(std::shared_ptr<detail::RpcCallState<Response>> state)
        : m_state(std::move(state)) {}

    std::shared_ptr<detail::RpcCallState<Response>> m_state;
};

template <typename Stub>
class Client {
  public:
    explicit Client(std::shared_ptr<RpcChannel> channel)
        : m_state(makeState(std::move(channel))) {
        static_assert(
            std::is_constructible_v<Stub, google::protobuf::RpcChannel*>,
            "Client requires a protobuf-generated RPC stub");
    }

    template <auto Method>
    auto call(
        const typename detail::RpcMethodTraits<
            decltype(Method)>::request_type& request,
        CallOptions options = {}) const {
        using Traits = detail::RpcMethodTraits<decltype(Method)>;
        using MethodService = typename Traits::service_type;
        using Response = typename Traits::response_type;
        static_assert(std::is_base_of_v<MethodService, Stub>,
                      "RPC method does not belong to this client stub");

        auto call_state =
            std::make_shared<detail::RpcCallState<Response>>();
        call_state->controller.SetTimeout(
            detail::timeoutMilliseconds(options.timeout));
        call_state->arm(call_state);

        std::invoke(Method, m_state->stub, &call_state->controller, &request,
                    &call_state->response, call_state.get());
        return RpcCall<Response>(std::move(call_state));
    }

    template <auto Method>
    auto callBlocking(
        const typename detail::RpcMethodTraits<
            decltype(Method)>::request_type& request,
        CallOptions options = {}) const {
        return std::move(call<Method>(request, std::move(options)))
            .get();
    }

    template <auto Method, typename Callback>
    void callAsync(
        const typename detail::RpcMethodTraits<
            decltype(Method)>::request_type& request,
        CallOptions options, Callback&& callback) const {
        // RpcChannel serializes request synchronously before call() returns,
        // so asynchronous completion never retains this borrowed reference.
        std::move(call<Method>(request, std::move(options)))
            .then(std::forward<Callback>(callback));
    }

    [[nodiscard]] const std::shared_ptr<RpcChannel>& channel() const noexcept {
        return m_state->channel;
    }

  private:
    static std::shared_ptr<detail::ClientState<Stub>>
    makeState(std::shared_ptr<RpcChannel> channel) {
        if (!channel) {
            throw std::invalid_argument("Client requires a non-null RpcChannel");
        }
        return std::make_shared<detail::ClientState<Stub>>(
            std::move(channel));
    }

    std::shared_ptr<detail::ClientState<Stub>> m_state;
};

template <typename Stub>
Client<Stub> MakeClient(std::shared_ptr<RpcChannel> channel) {
    return Client<Stub>(std::move(channel));
}

template <typename Stub>
Client<Stub> MakeClient(std::string_view service_or_address) {
    return MakeClient<Stub>(NewRpcChannel(service_or_address));
}

} // namespace rocket
