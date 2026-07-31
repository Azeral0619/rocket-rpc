#include "rocket/net/rpc/rpc_channel.h"

#include "rocket/common/config.h"
#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/service_registry.h"
#include "rocket/common/msg_id_util.h"
#include "rocket/common/runtime.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/tcp/tcp_client.h"
#include "rocket/net/timer_event.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rocket {

struct RpcChannel::RequestState {
    std::atomic<bool> finished{false};

    RpcController* controller{nullptr};
    google::protobuf::Message* response{nullptr};
    google::protobuf::Closure* done{nullptr};

    controller_s_ptr controller_owner;
    message_s_ptr request_owner;
    message_s_ptr response_owner;
    closure_s_ptr closure_owner;

    TcpClient::s_ptr client;
    TimerEvent::s_ptr timer;
    std::string msg_id;
};

RpcChannel::RpcChannel(NetAddr::s_ptr peer_addr)
    : m_peer_addr(std::move(peer_addr)), m_pool(GetDefaultPool()) {
    ROCKET_LOG_DEBUG("RpcChannel");
}

RpcChannel::RpcChannel(NetAddr::s_ptr peer_addr, RpcConnectionPool::s_ptr pool)
    : m_peer_addr(std::move(peer_addr)), m_pool(std::move(pool)) {
    ROCKET_LOG_DEBUG("RpcChannel with pool");
}

RpcChannel::RpcChannel(std::string_view service_name, RpcConnectionPool::s_ptr pool,
                       ServiceRegistry* registry)
    : m_service_name(service_name), m_pool(std::move(pool)), m_registry(registry) {
    ROCKET_LOG_DEBUG("RpcChannel service-aware: {}", service_name);
}

RpcChannel::~RpcChannel() { ROCKET_LOG_DEBUG("~RpcChannel"); }

RpcConnectionPool::s_ptr GetDefaultPool() {
    static auto pool = std::make_shared<RpcConnectionPool>();
    return pool;
}

bool RpcChannel::finishRpc(const std::shared_ptr<RequestState>& state,
                           std::function<void()> before_done,
                           bool cancel_pending_read) {
    if (!state) return false;

    bool expected = false;
    if (!state->finished.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
        return false;
    }

    if (state->timer) {
        if (state->client && state->client->getLoop()) {
            state->client->getLoop()->deleteTimerEvent(state->timer);
        } else {
            state->timer->cancel();
        }
        state->timer.reset();
    }
    if (cancel_pending_read && state->client && !state->msg_id.empty()) {
        state->client->cancelRead(state->msg_id);
    }

    if (before_done) before_done();

    // Keep shared Init() closures alive across re-entrant calls.  Standard
    // protobuf closures are owned by their caller and may self-delete in Run().
    auto closure_pin = state->closure_owner;
    auto* done = state->done;
    if (done) done->Run();
    return true;
}

// ── CallMethod: async RPC entry point ─────────────────────────────────

void RpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                            google::protobuf::RpcController* controller, const google::protobuf::Message* request,
                            google::protobuf::Message* response, google::protobuf::Closure* done) {
    (void)callMethodInternal(method, controller, request, response, done);
}

std::shared_ptr<RpcChannel::RequestState> RpcChannel::callMethodInternal(
    const google::protobuf::MethodDescriptor* method,
    google::protobuf::RpcController* controller,
    const google::protobuf::Message* request,
    google::protobuf::Message* response,
    google::protobuf::Closure* done) {
    auto* my_controller = dynamic_cast<RpcController*>(controller);

    if (my_controller == nullptr || method == nullptr || request == nullptr || response == nullptr) {
        ROCKET_LOG_ERROR("failed callmethod, RpcController convert error");
        if (my_controller != nullptr) {
            my_controller->SetError(error::kRpcChannelInit,
                                    "method, controller, request or response NULL");
        }
        if (done) done->Run();
        return nullptr;
    }

    auto state = std::make_shared<RequestState>();
    state->controller = my_controller;
    state->response = response;
    state->done = done;

    if (m_is_init.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(m_request_mutex);
        if (m_is_init.load(std::memory_order_relaxed)) {
            const bool legacy_matches = m_controller.get() == controller &&
                                        m_request.get() == request &&
                                        m_response.get() == response;
            if (legacy_matches) {
                state->controller_owner = std::move(m_controller);
                state->request_owner = std::move(m_request);
                state->response_owner = std::move(m_response);
                if (done == nullptr && m_closure) {
                    state->done = m_closure.get();
                    state->closure_owner = std::move(m_closure);
                } else if (m_closure.get() == done) {
                    state->closure_owner = std::move(m_closure);
                }

                // Init is only an ownership hand-off for the next matching
                // CallMethod.  Once captured, lifetime belongs to RequestState.
                m_controller.reset();
                m_request.reset();
                m_response.reset();
                m_closure.reset();
                m_is_init.store(false, std::memory_order_release);
            }
        }
    }

    auto req_protocol = std::make_shared<TinyPBProtocol>();

    // Acquire from pool, service discovery, or create standalone.
    std::shared_ptr<TcpClient> client;
    if (m_pool && m_registry && !m_service_name.empty()) {
        client = m_pool->acquireByService(m_service_name, m_registry);
        if (!client) {
            finishRpc(state, [state, service = m_service_name] {
                state->controller->SetError(error::kRpcPoolExhausted,
                                            "no available instance for " + service);
            });
            return state;
        }
        {
            std::lock_guard<std::mutex> lk(m_request_mutex);
            std::atomic_store_explicit(&m_client, client, std::memory_order_release);
            m_actual_peer = client->getPeerAddr();
        }
    } else {
        if (m_peer_addr == nullptr) {
            ROCKET_LOG_ERROR("failed get peer addr");
            finishRpc(state, [state] {
                state->controller->SetError(error::kRpcPeerAddr, "peer addr nullptr");
            });
            return state;
        }
        if (m_pool) {
            // A channel pins its concurrent calls to one established client.
            // This keeps connection selection off the RPC hot path while the
            // per-connection msg_id table multiplexes independent CallStates.
            auto cached = std::atomic_load_explicit(&m_client, std::memory_order_acquire);
            if (cached && cached->isConnected()) {
                client = std::move(cached);
            }
            if (!client) {
                auto acquired = m_pool->acquire(m_peer_addr);
                std::lock_guard<std::mutex> lk(m_request_mutex);
                cached = std::atomic_load_explicit(&m_client, std::memory_order_relaxed);
                if (cached && cached->isConnected()) {
                    client = std::move(cached);
                } else {
                    std::atomic_store_explicit(&m_client, acquired,
                                               std::memory_order_release);
                    client = std::move(acquired);
                    m_actual_peer = m_peer_addr;
                }
            }
            if (!client) {
                finishRpc(state, [state] {
                    state->controller->SetError(error::kRpcPoolExhausted,
                                                "connection pool exhausted");
                });
                return state;
            }
        } else {
            client = std::make_shared<TcpClient>(m_peer_addr,
                [] { return std::make_unique<TinyPBCoder>(); });
            std::atomic_store_explicit(&m_client, client, std::memory_order_release);
        }
    }
    state->client = client;

    if (my_controller->GetMsgId().empty()) {
        // A response callback is keyed by msg_id, so every in-flight RPC must
        // have its own ID.  RunTime::m_msgid is logging/trace context and can
        // outlive the request that populated it; inheriting it here lets two
        // calls made on the same thread overwrite each other's callbacks.
        req_protocol->m_msg_id = MsgIDUtil::GenMsgID();
        my_controller->SetMsgId(req_protocol->m_msg_id);
    } else {
        req_protocol->m_msg_id = my_controller->GetMsgId();
    }

    req_protocol->m_method_name = method->full_name();
    RunTime::GetRunTime()->m_msgid = req_protocol->m_msg_id;
    RunTime::GetRunTime()->m_method_name = req_protocol->m_method_name;

    ROCKET_LOG_INFO("{} | call method name [{}]", req_protocol->m_msg_id, req_protocol->m_method_name);

    if (!request->SerializeToString(&(req_protocol->m_pb_data))) {
        std::string err_info = "failed to serialize";
        ROCKET_LOG_ERROR("{} | {}, origin requeset [{}] ", req_protocol->m_msg_id, err_info,
                         request->ShortDebugString());
        finishRpc(state, [state, err_info] {
            state->controller->SetError(error::kFailedSerialize, err_info);
        });
        return state;
    }

    state->msg_id = req_protocol->m_msg_id;

    const int timeout_ms = std::max(1, my_controller->GetTimeout());
    state->timer = TimerEvent::create(timeout_ms, false, [state] {
        finishRpc(state, [state] {
            auto* ctrl = state->controller;
            ROCKET_LOG_INFO("{} | call rpc timeout arrive", ctrl->GetMsgId());
            ctrl->StartCancel();
            ctrl->SetError(error::kRpcCallTimeout,
                           "rpc call timeout " + std::to_string(ctrl->GetTimeout()));
        });
    });
    client->getLoop()->addTimerEvent(state->timer);

    // ── Send + readMessage ───────────────────────────────────────────
    auto doSendRead = [req_protocol, state, client] {
        if (state->finished.load(std::memory_order_acquire)) return;

        client->readMessage(
            req_protocol->m_msg_id, [state, client](const std::shared_ptr<AbstractProtocol>& msg) {
                finishRpc(state, [state, client, msg] {
                    auto rsp_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(msg);
                    if (!rsp_protocol) {
                        state->controller->SetError(error::kFailedDeserialize,
                                                    "invalid RPC response protocol");
                        return;
                    }

                    const auto peer = client->getPeerAddr();
                    const auto local = client->getLocalAddr();
                    ROCKET_LOG_INFO(
                        "{} | success get rpc response, call method name[{}], peer addr[{}], local addr[{}]",
                        rsp_protocol->m_msg_id, rsp_protocol->m_method_name,
                        peer ? peer->toString() : "?", local ? local->toString() : "?");

                    if (rsp_protocol->m_err_code != 0) {
                        ROCKET_LOG_ERROR("{} | call rpc method[{}] failed, error code[{}], error info[{}]",
                                         rsp_protocol->m_msg_id, rsp_protocol->m_method_name,
                                         rsp_protocol->m_err_code, rsp_protocol->m_err_info);
                        state->controller->SetError(rsp_protocol->m_err_code,
                                                    rsp_protocol->m_err_info);
                        return;
                    }

                    if (!state->response->ParseFromString(rsp_protocol->m_pb_data)) {
                        ROCKET_LOG_ERROR("{} | deserialize error", rsp_protocol->m_msg_id);
                        state->controller->SetError(error::kFailedDeserialize,
                                                    "response deserialize error");
                    }
                }, /*cancel_pending_read=*/false);
            });
        if (state->finished.load(std::memory_order_acquire)) {
            client->cancelRead(req_protocol->m_msg_id);
            return;
        }
        client->send(req_protocol);
    };

    if (m_pool) {
        doSendRead();  // already connected via pool::acquire → connectSync
    } else {
        client->connect([state, doSendRead, client]() mutable {
            if (client->getConnectErrorCode() != 0) {
                finishRpc(state, [state, client] {
                    state->controller->SetError(client->getConnectErrorCode(),
                                                client->getConnectErrorInfo());
                });
                return;
            }
            doSendRead();
        });
    }
    return state;
}

// ── CallMethodBlocking ─────────────────────────────────────────────────

void RpcChannel::Init(controller_s_ptr controller, message_s_ptr req, message_s_ptr res, closure_s_ptr done) {
    // Legacy ownership hand-off for the next matching CallMethod invocation.
    // The slot is consumed immediately by callMethodInternal, so completed
    // and in-flight calls never retain channel-global request state.
    std::lock_guard<std::mutex> lk(m_request_mutex);
    m_controller = std::move(controller);
    m_request = std::move(req);
    m_response = std::move(res);
    m_closure = std::move(done);
    m_is_init.store(true, std::memory_order_release);
}

google::protobuf::RpcController* RpcChannel::getController() {
    std::lock_guard<std::mutex> lk(m_request_mutex);
    return m_controller.get();
}
google::protobuf::Message* RpcChannel::getRequest() {
    std::lock_guard<std::mutex> lk(m_request_mutex);
    return m_request.get();
}
google::protobuf::Message* RpcChannel::getResponse() {
    std::lock_guard<std::mutex> lk(m_request_mutex);
    return m_response.get();
}
google::protobuf::Closure* RpcChannel::getClosure() {
    std::lock_guard<std::mutex> lk(m_request_mutex);
    return m_closure.get();
}
TcpClient* RpcChannel::getTcpClient() {
    return std::atomic_load_explicit(&m_client, std::memory_order_acquire).get();
}

NetAddr::s_ptr RpcChannel::FindAddr(std::string_view str) {
    if (IPNetAddr::checkValid(str))
        return std::make_shared<IPNetAddr>(str);
    const auto config = Config::getInstance().getConfig();
    auto it = config->rpc_stubs.find(str);
    if (it != config->rpc_stubs.end()) return it->second.addr;
    return nullptr;
}

int RpcChannel::CallMethodBlocking(const google::protobuf::MethodDescriptor* method,
                                   google::protobuf::RpcController* controller,
                                   const google::protobuf::Message* request,
                                   google::protobuf::Message* response,
                                   int timeout_ms) {
    struct BlockingState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
    };
    auto wait_state = std::make_shared<BlockingState>();

    auto* my_controller = dynamic_cast<RpcController*>(controller);
    if (my_controller) my_controller->SetTimeout(timeout_ms);

    struct BlockingClosure : public google::protobuf::Closure {
        std::shared_ptr<BlockingState> state;
        explicit BlockingClosure(std::shared_ptr<BlockingState> value)
            : state(std::move(value)) {}
        void Run() override {
            {
                std::lock_guard<std::mutex> lk(state->mutex);
                state->done = true;
            }
            state->cv.notify_one();
        }
    };
    auto closure = std::make_shared<BlockingClosure>(wait_state);

    Init(std::shared_ptr<google::protobuf::RpcController>(controller, [](auto*) {}),
         std::shared_ptr<google::protobuf::Message>(const_cast<google::protobuf::Message*>(request), [](auto*) {}),
         std::shared_ptr<google::protobuf::Message>(response, [](auto*) {}),
         closure);

    auto request_state =
        callMethodInternal(method, controller, request, response, closure.get());

    bool local_timeout = false;
    {
        std::unique_lock<std::mutex> lk(wait_state->mutex);
        if (!wait_state->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                     [&] { return wait_state->done; })) {
            local_timeout = true;
        }
    }

    if (local_timeout) {
        const bool cancelled = finishRpc(request_state, [my_controller] {
            if (my_controller) {
                my_controller->StartCancel();
                my_controller->SetError(error::kRpcCallTimeout,
                                        "CallMethodBlocking timeout");
            }
        });

        if (!cancelled && request_state) {
            // A response or timer already claimed completion.  It may still
            // be parsing into caller-owned objects, so wait for its closure
            // before returning those objects to the caller.
            std::unique_lock<std::mutex> lk(wait_state->mutex);
            wait_state->cv.wait(lk, [&] { return wait_state->done; });
        }
    }

    // Release connection on the worker thread (not EventLoop thread).
    TcpClient::s_ptr client;
    NetAddr::s_ptr actual_peer;
    {
        std::lock_guard<std::mutex> lk(m_request_mutex);
        client = std::atomic_load_explicit(&m_client, std::memory_order_relaxed);
        actual_peer = m_actual_peer ? m_actual_peer : m_peer_addr;
    }
    if (m_pool && client) m_pool->release(actual_peer, client);

    if (local_timeout) return -1;
    if (my_controller && my_controller->GetErrorCode() != 0)
        return my_controller->GetErrorCode();
    return 0;
}

} // namespace rocket
