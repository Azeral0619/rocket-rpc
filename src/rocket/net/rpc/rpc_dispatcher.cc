#include "rocket/net/rpc/rpc_dispatcher.h"

#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/runtime.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/tcp/tcp_connection.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rocket {

namespace {

struct ServerCallFreeNode {
    ServerCallFreeNode* next{nullptr};
};

class ServerCallPool {
  public:
    ~ServerCallPool() {
        while (m_head != nullptr) {
            auto* node = m_head;
            m_head = node->next;
            ::operator delete(node);
        }
    }

    void* allocate(std::size_t size) {
        if (m_head == nullptr) return ::operator new(size);
        auto* node = m_head;
        m_head = node->next;
        --m_size;
        return node;
    }

    void deallocate(void* ptr) noexcept {
        constexpr std::size_t kMaxCachedCalls = 512;
        if (m_size >= kMaxCachedCalls) {
            ::operator delete(ptr);
            return;
        }
        auto* node = static_cast<ServerCallFreeNode*>(ptr);
        node->next = m_head;
        m_head = node;
        ++m_size;
    }

  private:
    ServerCallFreeNode* m_head{nullptr};
    std::size_t m_size{0};
};

thread_local ServerCallPool t_server_call_pool;

// One allocation owns all state retained by an asynchronous protobuf method.
// The old path separately allocated shared_ptr control blocks for request,
// response and controller plus a heap std::function closure.
class ServerCallState final : public google::protobuf::Closure {
  public:
    ServerCallState(
        std::shared_ptr<google::protobuf::Service> service,
        const google::protobuf::MethodDescriptor* method,
        std::unique_ptr<google::protobuf::Message> request,
        std::unique_ptr<google::protobuf::Message> response,
        TinyPBProtocol::s_ptr request_protocol,
        TinyPBProtocol::s_ptr response_protocol,
        TcpConnection::s_ptr connection)
        : m_service(std::move(service)), m_method(method),
          m_request(std::move(request)), m_response(std::move(response)),
          m_request_protocol(std::move(request_protocol)),
          m_response_protocol(std::move(response_protocol)),
          m_connection(std::move(connection)) {
        m_controller.SetLocalAddr(m_connection->getLocalAddr());
        m_controller.SetPeerAddr(m_connection->getPeerAddr());
        m_controller.SetMsgId(m_request_protocol->m_msg_id);
    }

    static void* operator new(std::size_t size) {
        return t_server_call_pool.allocate(size);
    }

    static void operator delete(void* ptr) noexcept {
        t_server_call_pool.deallocate(ptr);
    }

    void markInFlight() {
        m_connection->incrInFlight();
        m_in_flight = true;
    }

    void invoke() {
        auto* runtime = RunTime::GetRunTime();
        runtime->m_msgid = m_request_protocol->m_msg_id;
        runtime->m_method_name = m_method->name();

        // CallMethod may complete synchronously and delete this through Run().
        // Do not access members after the call returns.
        m_service->CallMethod(m_method, &m_controller, m_request.get(),
                              m_response.get(), this);
    }

    void Run() override {
        std::unique_ptr<ServerCallState> self(this);
        finishInFlight();

        if (!m_response->SerializeToString(&m_response_protocol->m_pb_data)) {
            ROCKET_LOG_ERROR("{} | serialize error, origin message [{}]",
                             m_request_protocol->m_msg_id,
                             m_response->ShortDebugString());
            m_response_protocol->m_err_code = error::kFailedSerialize;
            m_response_protocol->m_err_info = "serialize error";
            m_response_protocol->m_err_info_len =
                static_cast<std::int32_t>(
                    m_response_protocol->m_err_info.length());
        } else {
            m_response_protocol->m_err_code = 0;
            m_response_protocol->m_err_info.clear();
            ROCKET_LOG_INFO(
                "{} | dispatch success, request[{}], response[{}]",
                m_request_protocol->m_msg_id, m_request->ShortDebugString(),
                m_response->ShortDebugString());
        }
        m_connection->send(std::move(m_response_protocol));
    }

    void reject(std::int32_t err_code, std::string_view err_info) {
        std::unique_ptr<ServerCallState> self(this);
        finishInFlight();
        RpcDispatcher::setTinyPBError(m_response_protocol, err_code, err_info);
        m_connection->send(std::move(m_response_protocol));
    }

  private:
    void finishInFlight() noexcept {
        if (!m_in_flight) return;
        m_in_flight = false;
        m_connection->decrInFlight();
    }

    std::shared_ptr<google::protobuf::Service> m_service;
    const google::protobuf::MethodDescriptor* m_method;
    std::unique_ptr<google::protobuf::Message> m_request;
    std::unique_ptr<google::protobuf::Message> m_response;
    TinyPBProtocol::s_ptr m_request_protocol;
    TinyPBProtocol::s_ptr m_response_protocol;
    TcpConnection::s_ptr m_connection;
    RpcController m_controller;
    bool m_in_flight{false};
};

} // namespace

RpcDispatcher::RpcDispatcher(std::size_t worker_threads, std::size_t max_pending_tasks)
    : m_worker_pool(worker_threads, max_pending_tasks) {
}

void RpcDispatcher::dispatch(AbstractProtocol::s_ptr request, AbstractProtocol::s_ptr response,
                             const TcpConnection::s_ptr& conn) {

    auto req_protocol =
        std::dynamic_pointer_cast<TinyPBProtocol>(std::move(request));
    auto rsp_protocol =
        std::dynamic_pointer_cast<TinyPBProtocol>(std::move(response));

    if (!req_protocol || !rsp_protocol) {
        // Not a TinyPB message — can't handle
        return;
    }

    const std::string_view method_full_name = req_protocol->m_method_name;
    std::string_view service_name;
    std::string_view method_name;

    rsp_protocol->m_msg_id = req_protocol->m_msg_id;
    rsp_protocol->m_method_name = req_protocol->m_method_name;

    auto sendError = [&](int32_t err_code, std::string_view err_info) {
        setTinyPBError(rsp_protocol, err_code, err_info);
        conn->send(rsp_protocol);
    };

    if (m_stopping.load(std::memory_order_acquire)) {
        sendError(error::kRpcShutdown, "server is shutting down");
        return;
    }

    if (!parseServiceFullName(method_full_name, service_name, method_name)) {
        sendError(error::kParseServiceName, "parse service name error");
        return;
    }

    auto it = m_service_map.find(service_name);
    if (it == m_service_map.end()) {
        ROCKET_LOG_ERROR("{} | service name[{}] not found", req_protocol->m_msg_id, service_name);
        sendError(error::kServiceNotFound, "service not found");
        return;
    }

    const auto& service = it->second;
    const auto* method = service->GetDescriptor()->FindMethodByName(method_name);
    if (method == nullptr) {
        ROCKET_LOG_ERROR("{} | method name[{}] not found in service[{}]", req_protocol->m_msg_id, method_name,
                         service_name);
        sendError(error::kMethodNotFound, "method not found");
        return;
    }

    auto request_message = std::unique_ptr<google::protobuf::Message>(
        service->GetRequestPrototype(method).New());
    if (!request_message->ParseFromString(req_protocol->m_pb_data)) {
        ROCKET_LOG_ERROR("{} | deserialize error", req_protocol->m_msg_id);
        sendError(error::kFailedDeserialize, "deserialize error");
        return;
    }

    ROCKET_LOG_INFO("{} | get rpc request[{}]", req_protocol->m_msg_id,
                    request_message->ShortDebugString());

    auto response_message = std::unique_ptr<google::protobuf::Message>(
        service->GetResponsePrototype(method).New());

    auto* call = new ServerCallState(
        service, method, std::move(request_message),
        std::move(response_message), std::move(req_protocol),
        std::move(rsp_protocol), conn);

    // Track in-flight for graceful shutdown.
    call->markInFlight();

    const auto mode = executionModeFor(method->full_name());
    if (mode == RpcExecutionMode::Inline) {
        call->invoke();
        return;
    }

    const bool queued =
        m_worker_pool.tryExecute([call] { call->invoke(); });
    if (!queued) {
        const bool stopping = m_stopping.load(std::memory_order_acquire);
        const auto err_code =
            stopping ? error::kRpcShutdown : error::kRpcServerOverloaded;
        const std::string_view err_info =
            stopping ? "server is shutting down" : "server worker queue is full";
        ROCKET_LOG_WARN("server call rejected: {}", err_info);
        call->reject(err_code, err_info);
    }
}

bool RpcDispatcher::parseServiceFullName(std::string_view full_name, std::string_view& service_name,
                                         std::string_view& method_name) {
    if (full_name.empty()) {
        ROCKET_LOG_ERROR("full name empty");
        return false;
    }
    size_t i = full_name.rfind('.');
    if (i == std::string::npos) {
        ROCKET_LOG_ERROR("not find . in full name [{}]", full_name);
        return false;
    }
    service_name = full_name.substr(0, i);
    method_name = full_name.substr(i + 1, full_name.length() - i - 1);
    return true;
}

void RpcDispatcher::registerService(Services_ptr service) {
    std::string service_name(service->GetDescriptor()->full_name());
    m_service_map[service_name] = std::move(service);
}

void RpcDispatcher::setDefaultExecutionMode(RpcExecutionMode mode) noexcept {
    m_default_execution_mode = mode;
}

bool RpcDispatcher::setMethodExecutionMode(std::string full_method_name,
                                           RpcExecutionMode mode) {
    if (full_method_name.empty()) return false;
    m_method_execution_modes.insert_or_assign(std::move(full_method_name), mode);
    return true;
}

RpcExecutionMode
RpcDispatcher::executionModeFor(std::string_view full_method_name) const {
    if (m_method_execution_modes.empty()) return m_default_execution_mode;
    const auto it = m_method_execution_modes.find(full_method_name);
    return it == m_method_execution_modes.end() ? m_default_execution_mode
                                                 : it->second;
}

std::size_t RpcDispatcher::pendingWorkerTasks() const {
    return m_worker_pool.pendingCount();
}

void RpcDispatcher::stop() {
    m_stopping.store(true, std::memory_order_release);
    m_worker_pool.stopAndDrain();
}

void RpcDispatcher::setTinyPBError(TinyPBProtocol::s_ptr msg, int32_t err_code, std::string_view err_info) {
    msg->m_err_code = err_code;
    msg->m_err_info = err_info;
    msg->m_err_info_len = static_cast<std::int32_t>(err_info.length());
}

} // namespace rocket
