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
    std::uint64_t generation{0};

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
                           std::function<void()> before_done) {
    if (!state) return false;

    bool expected = false;
    if (!state->finished.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
        return false;
    }

    if (state->timer) {
        state->timer->cancel();
        state->timer.reset();
    }
    if (state->client && !state->msg_id.empty()) {
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
    auto* my_controller = dynamic_cast<RpcController*>(controller);

    bool has_in_flight = false;
    {
        std::lock_guard<std::mutex> lk(m_request_mutex);
        has_in_flight = m_active_request &&
                        !m_active_request->finished.load(std::memory_order_acquire);
    }
    if (has_in_flight) {
        if (my_controller) {
            my_controller->SetError(error::kRpcChannelInit,
                                    "RpcChannel already has an in-flight request");
        }
        if (done) done->Run();
        return;
    }

    if (my_controller == nullptr || method == nullptr || request == nullptr || response == nullptr) {
        ROCKET_LOG_ERROR("failed callmethod, RpcController convert error");
        if (my_controller != nullptr) {
            my_controller->SetError(error::kRpcChannelInit,
                                    "method, controller, request or response NULL");
        }
        if (done) done->Run();
        return;
    }

    auto state = std::make_shared<RequestState>();
    state->controller = my_controller;
    state->response = response;
    state->done = done;

    bool lost_start_race = false;
    {
        std::lock_guard<std::mutex> lk(m_request_mutex);
        lost_start_race = m_active_request &&
                          !m_active_request->finished.load(std::memory_order_acquire);
        if (!lost_start_race) {
            state->generation = ++m_next_generation;

            if (m_is_init) {
                const bool legacy_matches = m_controller.get() == controller &&
                                            m_request.get() == request &&
                                            m_response.get() == response;
                if (m_controller.get() == controller) state->controller_owner = m_controller;
                if (m_request.get() == request) state->request_owner = m_request;
                if (m_response.get() == response) state->response_owner = m_response;
                if (done == nullptr && legacy_matches && m_closure) {
                    state->done = m_closure.get();
                    state->closure_owner = m_closure;
                } else if (m_closure.get() == done) {
                    state->closure_owner = m_closure;
                }
            }
            m_active_request = state;
        }
    }
    if (lost_start_race) {
        my_controller->SetError(error::kRpcChannelInit,
                                "RpcChannel already has an in-flight request");
        if (done) done->Run();
        return;
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
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_request_mutex);
            m_client = client;
            m_actual_peer = client->getPeerAddr();
        }
    } else {
        if (m_peer_addr == nullptr) {
            ROCKET_LOG_ERROR("failed get peer addr");
            finishRpc(state, [state] {
                state->controller->SetError(error::kRpcPeerAddr, "peer addr nullptr");
            });
            return;
        }
        if (m_pool) {
            client = m_pool->acquire(m_peer_addr);
            if (!client) {
                finishRpc(state, [state] {
                    state->controller->SetError(error::kRpcPoolExhausted,
                                                "connection pool exhausted");
                });
                return;
            }
            {
                std::lock_guard<std::mutex> lk(m_request_mutex);
                m_client = client;
                m_actual_peer = m_peer_addr;
            }
        } else {
            client = std::make_shared<TcpClient>(m_peer_addr,
                [] { return std::make_unique<TinyPBCoder>(); });
            std::lock_guard<std::mutex> lk(m_request_mutex);
            m_client = client;
        }
    }
    state->client = client;

    if (my_controller->GetMsgId().empty()) {
        std::string msg_id = RunTime::GetRunTime()->m_msgid;
        if (!msg_id.empty()) {
            req_protocol->m_msg_id = msg_id;
            my_controller->SetMsgId(msg_id);
        } else {
            req_protocol->m_msg_id = MsgIDUtil::GenMsgID();
            my_controller->SetMsgId(req_protocol->m_msg_id);
        }
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
        return;
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
                });
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
}

// ── CallMethodBlocking ─────────────────────────────────────────────────

void RpcChannel::Init(controller_s_ptr controller, message_s_ptr req, message_s_ptr res, closure_s_ptr done) {
    // Guard: refuse to overwrite while an RPC is still in flight.
    std::lock_guard<std::mutex> lk(m_request_mutex);
    if (m_active_request &&
        !m_active_request->finished.load(std::memory_order_acquire)) {
        return;
    }
    m_controller = std::move(controller);
    m_request = std::move(req);
    m_response = std::move(res);
    m_closure = std::move(done);
    m_is_init = true;
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
    std::lock_guard<std::mutex> lk(m_request_mutex);
    return m_client.get();
}

void RpcChannel::clearInitState(const std::shared_ptr<RequestState>& state) {
    std::lock_guard<std::mutex> lk(m_request_mutex);
    if (!state || m_active_request != state) return;
    m_active_request.reset();
    m_is_init = false;
    m_controller.reset();
    m_request.reset();
    m_response.reset();
    m_closure.reset();
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

    CallMethod(method, controller, request, response, closure.get());

    std::shared_ptr<RequestState> request_state;
    {
        std::lock_guard<std::mutex> lk(m_request_mutex);
        if (m_active_request && m_active_request->controller == my_controller) {
            request_state = m_active_request;
        }
    }

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
        client = m_client;
        actual_peer = m_actual_peer ? m_actual_peer : m_peer_addr;
    }
    if (m_pool && client) m_pool->release(actual_peer, client);

    clearInitState(request_state);

    if (local_timeout) return -1;
    if (my_controller && my_controller->GetErrorCode() != 0)
        return my_controller->GetErrorCode();
    return 0;
}

} // namespace rocket
