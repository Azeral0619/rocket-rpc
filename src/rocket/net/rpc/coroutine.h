#pragma once

#include "rocket/common/runtime.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_controller.h"

#include <coroutine>
#include <exception>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <memory>
#include <string>
#include <utility>

namespace rocket {

// ─── Task<T> — lazy coroutine return type ─────────────────────────────
//
// Usage:
//   Task<makeOrderResponse> doOrder(RpcChannel::s_ptr ch) {
//       makeOrderRequest req;
//       req.set_price(100);
//       auto rsp = co_await coCall<makeOrderResponse>(ch, method, &req);
//       co_return rsp;
//   }
//
// The coroutine starts suspended.  Call .run() on the returned Task to
// drive it to completion (blocks the calling thread).
//
// Errors are stored in the Task rather than thrown.

template <typename T = void>
class Task;

namespace detail {

// Shared state for coCall awaiter.  Lives on the heap, owned by the
// closure that RpcChannel invokes.
template <typename Response>
struct CoCallState {
    std::coroutine_handle<> handle;         // coroutine to resume
    Response result;
    int err_code{0};
    std::string err_info;
    RpcController controller;

    // Saved RunTime context (msgid + method_name) captured before the call.
    std::string saved_msgid;
    std::string saved_method_name;
};

} // namespace detail

// ─── coCall<Response> awaitable ──────────────────────────────────────

template <typename Response>
class CoCallAwaitable {
  public:
    CoCallAwaitable(RpcChannel::s_ptr channel,
                    const google::protobuf::MethodDescriptor* method,
                    const google::protobuf::Message* request,
                    int timeout_ms = 3000)
        : m_channel(std::move(channel)), m_method(method),
          m_request(request), m_timeout_ms(timeout_ms) {}

    [[nodiscard]] bool await_ready() noexcept { return false; }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) {
        m_state = std::make_shared<detail::CoCallState<Response>>();
        m_state->handle = h;

        auto* rt = RunTime::GetRunTime();
        if (rt) {
            m_state->saved_msgid = rt->m_msgid;
            m_state->saved_method_name = rt->m_method_name;
        }

        m_state->controller.SetTimeout(m_timeout_ms);

        // Protobuf's NewCallback doesn't support capturing lambdas.
        // Use a hand-rolled Closure that holds the shared state.
        struct ResumeClosure : public google::protobuf::Closure {
            std::shared_ptr<detail::CoCallState<Response>> state;
            explicit ResumeClosure(std::shared_ptr<detail::CoCallState<Response>> s)
                : state(std::move(s)) {}
            void Run() override {
                auto* rt = RunTime::GetRunTime();
                if (rt) {
                    rt->m_msgid = std::move(state->saved_msgid);
                    rt->m_method_name = std::move(state->saved_method_name);
                }
                if (state->handle) {
                    auto handle = state->handle;
                    state->handle = nullptr;
                    handle.resume();
                }
            }
        };
        auto closure = new ResumeClosure(m_state);

        m_channel->CallMethod(m_method, &m_state->controller, m_request,
                              &m_state->result, closure);
    }

    Response await_resume() {
        return std::move(m_state->result);
    }

  private:
    RpcChannel::s_ptr m_channel;
    const google::protobuf::MethodDescriptor* m_method;
    const google::protobuf::Message* m_request;
    int m_timeout_ms{3000};
    std::shared_ptr<detail::CoCallState<Response>> m_state;
};

template <typename Response>
CoCallAwaitable<Response> coCall(RpcChannel::s_ptr channel,
                                  const google::protobuf::MethodDescriptor* method,
                                  const google::protobuf::Message* request,
                                  int timeout_ms = 3000) {
    return CoCallAwaitable<Response>(std::move(channel), method, request, timeout_ms);
}

// ─── Task<T> implementation ───────────────────────────────────────────

template <typename T>
class Task {
  public:
    struct promise_type {
        T result;
        int err_code{0};
        std::string err_info;
        std::exception_ptr exception;

        [[nodiscard]] Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }

        void unhandled_exception() { exception = std::current_exception(); }

        template <typename U>
            requires std::convertible_to<U, T>
        void return_value(U&& value) {
            result = std::forward<U>(value);
        }

        void setError(int code, std::string info) {
            err_code = code;
            err_info = std::move(info);
        }
    };

    Task(Task&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (m_handle) m_handle.destroy();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    ~Task() { if (m_handle) m_handle.destroy(); }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // Drive the coroutine to completion.  Returns the result.
    // Throws if the coroutine threw an exception.
    T run() {
        if (!m_handle) return T{};
        m_handle.resume(); // enter coroutine body
        if (m_handle.promise().exception) {
            std::rethrow_exception(m_handle.promise().exception);
        }
        return std::move(m_handle.promise().result);
    }

    [[nodiscard]] int errorCode() const noexcept {
        return m_handle ? m_handle.promise().err_code : 0;
    }

    [[nodiscard]] const std::string& errorInfo() const {
        static const std::string empty;
        return m_handle ? m_handle.promise().err_info : empty;
    }

    [[nodiscard]] bool done() const noexcept {
        return m_handle && m_handle.done();
    }

  private:
    explicit Task(std::coroutine_handle<promise_type> h) : m_handle(h) {}
    std::coroutine_handle<promise_type> m_handle;
};

} // namespace rocket
