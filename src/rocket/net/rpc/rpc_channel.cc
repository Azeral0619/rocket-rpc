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

// ── finishRpc: idempotent completion via atomic flag ──────────────────
// Called from response callback or timer callback (both on EventLoop thread).
// Only the first caller runs the closure and notifies the waiter.
void RpcChannel::finishRpc() {
    bool expected = false;
    if (!m_rpc_finished.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
        return;  // already finished
    }
    if (m_closure) {
        auto pin = m_closure;  // keep alive across re-entrant Init() in Run()
        pin->Run();
    }
}

// ── CallMethod: async RPC entry point ─────────────────────────────────

void RpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                            google::protobuf::RpcController* controller, const google::protobuf::Message* request,
                            google::protobuf::Message* response, google::protobuf::Closure* /*done*/) {

    auto req_protocol = std::make_shared<TinyPBProtocol>();

    auto* my_controller = dynamic_cast<RpcController*>(controller);
    if (my_controller == nullptr || request == nullptr || response == nullptr) {
        ROCKET_LOG_ERROR("failed callmethod, RpcController convert error");
        if (my_controller != nullptr) {
            my_controller->SetError(error::kRpcChannelInit, "controller or request or response NULL");
        }
        finishRpc();
        return;
    }

    // Cancel the PREVIOUS timer *before* resetting m_rpc_finished.
    // Otherwise the old timer can fire in the window between the reset and
    // the cancel below, spuriously invoking the new callback.
    if (m_timer_event) {
        m_timer_event->cancel();
        m_timer_event.reset();
    }

    // Reset the per-request finish flag BEFORE starting the new request.
    m_rpc_finished.store(false, std::memory_order_release);

    // Acquire from pool, service discovery, or create standalone.
    std::shared_ptr<TcpClient> client;
    if (m_pool && m_registry && !m_service_name.empty()) {
        client = m_pool->acquireByService(m_service_name, m_registry);
        if (!client) {
            my_controller->SetError(error::kRpcPoolExhausted, "no available instance for " + m_service_name);
            finishRpc();
            return;
        }
        m_client = client;
        m_actual_peer = client->getPeerAddr();
    } else {
        if (m_peer_addr == nullptr) {
            ROCKET_LOG_ERROR("failed get peer addr");
            my_controller->SetError(error::kRpcPeerAddr, "peer addr nullptr");
            finishRpc();
            return;
        }
        if (m_pool) {
            client = m_pool->acquire(m_peer_addr);
            if (!client) {
                my_controller->SetError(error::kRpcPoolExhausted, "connection pool exhausted");
                finishRpc();
                return;
            }
            m_client = client;
            m_actual_peer = m_peer_addr;
        } else {
            client = std::make_shared<TcpClient>(m_peer_addr,
                [] { return std::make_unique<TinyPBCoder>(); });
            m_client = client;
        }
    }

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

    if (!m_is_init) {
        std::string err_info = "RpcChannel not call init()";
        my_controller->SetError(error::kRpcChannelInit, err_info);
        ROCKET_LOG_ERROR("{} | {}, RpcChannel not init ", req_protocol->m_msg_id, err_info);
        finishRpc();
        return;
    }

    if (!request->SerializeToString(&(req_protocol->m_pb_data))) {
        std::string err_info = "failde to serialize";
        my_controller->SetError(error::kFailedSerialize, err_info);
        ROCKET_LOG_ERROR("{} | {}, origin requeset [{}] ", req_protocol->m_msg_id, err_info,
                         request->ShortDebugString());
        finishRpc();
        return;
    }

    auto channel = shared_from_this();

    // ── Timer: fires finishRpc() with timeout error ──────────────────
    // Capture controller as shared_ptr so the timer is safe even if
    // the channel is re-init'd with a different controller before this fires.
    {
        auto ctrl = m_controller;  // shared_ptr pin
        int timeout_ms = my_controller->GetTimeout();
        m_timer_event = TimerEvent::create(
            timeout_ms, false, [ctrl, channel]() mutable {
                if (channel->m_rpc_finished.load(std::memory_order_acquire)) return;

                auto* mc = static_cast<RpcController*>(ctrl.get());
                ROCKET_LOG_INFO("{} | call rpc timeout arrive", mc->GetMsgId());
                mc->StartCancel();
                mc->SetError(error::kRpcCallTimeout,
                             "rpc call timeout " + std::to_string(mc->GetTimeout()));
                channel->finishRpc();
                channel.reset();
                ctrl.reset();
            });
    }

    client->getLoop()->addTimerEvent(m_timer_event);

    // ── Send + readMessage ───────────────────────────────────────────
    // Capture channel (shared_ptr), not raw this — the timer callback may
    // destroy the RpcChannel before this read-callback fires.
    auto doSendRead = [req_protocol, channel, client] {
        auto* mc = dynamic_cast<RpcController*>(channel->getController());
        client->send(req_protocol);
        client->readMessage(
            req_protocol->m_msg_id, [channel, mc, client](const std::shared_ptr<AbstractProtocol>& msg) mutable {
                // Guard: if timer already finished, ignore late response.
                if (channel->m_rpc_finished.load(std::memory_order_acquire)) return;

                auto rsp_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(msg);
                ROCKET_LOG_INFO(
                    "{} | success get rpc response, call method name[{}], peer addr[{}], local addr[{}]",
                    rsp_protocol->m_msg_id, rsp_protocol->m_method_name,
                    client->getPeerAddr()->toString(), client->getLocalAddr()->toString());

                if (!(channel->getResponse()->ParseFromString(rsp_protocol->m_pb_data))) {
                    ROCKET_LOG_ERROR("{} | serialize error", rsp_protocol->m_msg_id);
                    mc->SetError(error::kFailedSerialize, "serialize error");
                    channel->finishRpc();
                    return;
                }

                if (rsp_protocol->m_err_code != 0) {
                    ROCKET_LOG_ERROR("{} | call rpc methood[{}] failed, error code[{}], error info[{}]",
                                     rsp_protocol->m_msg_id, rsp_protocol->m_method_name,
                                     rsp_protocol->m_err_code, rsp_protocol->m_err_info);
                    mc->SetError(rsp_protocol->m_err_code, rsp_protocol->m_err_info);
                    channel->finishRpc();
                    return;
                }

                ROCKET_LOG_INFO("{} | call rpc success, call method name[{}], peer addr[{}], local addr[{}]",
                                rsp_protocol->m_msg_id, rsp_protocol->m_method_name,
                                client->getPeerAddr()->toString(), client->getLocalAddr()->toString());

                channel->finishRpc();
            });
    };

    if (m_pool) {
        doSendRead();  // already connected via pool::acquire → connectSync
    } else {
        client->connect([this, doSendRead, client]() mutable {
            if (client->getConnectErrorCode() != 0) {
                auto* mc = dynamic_cast<RpcController*>(getController());
                mc->SetError(client->getConnectErrorCode(), client->getConnectErrorInfo());
                finishRpc();
                return;
            }
            doSendRead();
        });
    }
}

// ── CallMethodBlocking ─────────────────────────────────────────────────

void RpcChannel::Init(controller_s_ptr controller, message_s_ptr req, message_s_ptr res, closure_s_ptr done) {
    // Guard: refuse to overwrite while an RPC is still in flight.
    if (m_is_init && !m_rpc_finished.load(std::memory_order_acquire)) {
        return;
    }
    m_controller = std::move(controller);
    m_request = std::move(req);
    m_response = std::move(res);
    m_closure = std::move(done);
    m_is_init = true;
}

google::protobuf::RpcController* RpcChannel::getController() { return m_controller.get(); }
google::protobuf::Message* RpcChannel::getRequest() { return m_request.get(); }
google::protobuf::Message* RpcChannel::getResponse() { return m_response.get(); }
google::protobuf::Closure* RpcChannel::getClosure() { return m_closure.get(); }
TcpClient* RpcChannel::getTcpClient() { return m_client.get(); }

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
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    auto* my_controller = dynamic_cast<RpcController*>(controller);
    if (my_controller) my_controller->SetTimeout(timeout_ms);

    struct BlockingClosure : public google::protobuf::Closure {
        std::mutex& mtx; std::condition_variable& cv; bool& done;
        BlockingClosure(std::mutex& m, std::condition_variable& c, bool& d) : mtx(m), cv(c), done(d) {}
        void Run() override { std::lock_guard<std::mutex> lk(mtx); done = true; cv.notify_one(); }
    };
    auto closure = std::make_shared<BlockingClosure>(mtx, cv, done);

    Init(std::shared_ptr<google::protobuf::RpcController>(controller, [](auto*) {}),
         std::shared_ptr<google::protobuf::Message>(const_cast<google::protobuf::Message*>(request), [](auto*) {}),
         std::shared_ptr<google::protobuf::Message>(response, [](auto*) {}),
         closure);

    CallMethod(method, controller, request, response, nullptr);

    {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return done; })) {
            if (my_controller) my_controller->SetError(error::kRpcCallTimeout, "CallMethodBlocking timeout");
            m_is_init = false; m_controller.reset(); m_request.reset(); m_response.reset(); m_closure.reset();
            return -1;
        }
    }

    // Release connection on the worker thread (not EventLoop thread).
    if (m_pool && m_client) {
        m_pool->release(m_actual_peer ? m_actual_peer : m_peer_addr, m_client);
    }

    m_is_init = false; m_controller.reset(); m_request.reset(); m_response.reset(); m_closure.reset();

    if (my_controller && my_controller->GetErrorCode() != 0)
        return my_controller->GetErrorCode();
    return 0;
}

} // namespace rocket
