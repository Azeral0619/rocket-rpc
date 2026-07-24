#include "rocket/net/rpc/rpc_dispatcher.h"

#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/runtime.h"
#include "rocket/net/rpc/rpc_closure.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/tcp/tcp_connection.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rocket {

RpcDispatcher::RpcDispatcher(std::size_t worker_threads)
    : m_worker_pool(worker_threads) {
}

void RpcDispatcher::dispatch(AbstractProtocol::s_ptr request, AbstractProtocol::s_ptr response,
                             const TcpConnection::s_ptr& conn) {

    auto req_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(request);
    auto rsp_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(response);

    if (!req_protocol || !rsp_protocol) {
        // Not a TinyPB message — can't handle
        return;
    }

    std::string method_full_name = req_protocol->m_method_name;
    std::string_view service_name;
    std::string_view method_name;

    rsp_protocol->m_msg_id = req_protocol->m_msg_id;
    rsp_protocol->m_method_name = req_protocol->m_method_name;

    auto sendError = [&](int32_t err_code, std::string_view err_info) {
        setTinyPBError(rsp_protocol, err_code, err_info);
        conn->send(rsp_protocol);
    };

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

    // shared_ptr directly from prototype (avoid unique_ptr → shared_ptr conversion).
    auto req_msg_ptr = std::shared_ptr<google::protobuf::Message>(service->GetRequestPrototype(method).New());
    if (!req_msg_ptr->ParseFromString(req_protocol->m_pb_data)) {
        ROCKET_LOG_ERROR("{} | deserialize error", req_protocol->m_msg_id);
        sendError(error::kFailedDeserialize, "deserialize error");
        return;
    }

    ROCKET_LOG_INFO("{} | get rpc request[{}]", req_protocol->m_msg_id, req_msg_ptr->ShortDebugString());

    auto rsp_msg_ptr = std::shared_ptr<google::protobuf::Message>(service->GetResponsePrototype(method).New());

    auto controller_ptr = std::make_shared<RpcController>();
    controller_ptr->SetLocalAddr(conn->getLocalAddr());
    controller_ptr->SetPeerAddr(conn->getPeerAddr());
    controller_ptr->SetMsgId(req_protocol->m_msg_id);

    RunTime::GetRunTime()->m_msgid = req_protocol->m_msg_id;
    RunTime::GetRunTime()->m_method_name = method_name;

    // Track in-flight for graceful shutdown.
    conn->incrInFlight();

    auto closure = std::make_shared<RpcClosure>(
        nullptr, [req_msg_ptr, rsp_msg_ptr, req_protocol, rsp_protocol, conn, controller_ptr, this]() mutable {
            conn->decrInFlight();

            if (!rsp_msg_ptr->SerializeToString(&(rsp_protocol->m_pb_data))) {
                ROCKET_LOG_ERROR("{} | serialize error, origin message [{}]", req_protocol->m_msg_id,
                                 rsp_msg_ptr->ShortDebugString());
                setTinyPBError(rsp_protocol, error::kFailedSerialize, "serialize error");
            } else {
                rsp_protocol->m_err_code = 0;
                rsp_protocol->m_err_info = "";
                ROCKET_LOG_INFO("{} | dispatch success, request[{}], response[{}]", req_protocol->m_msg_id,
                                req_msg_ptr->ShortDebugString(), rsp_msg_ptr->ShortDebugString());
            }
            conn->send(rsp_protocol);
        });

    // Fast path: run directly on the IO thread (brpc/gRPC style).
    // Slow methods can offload themselves by not calling done->Run()
    // synchronously — the framework waits for the callback.
    service->CallMethod(method, controller_ptr.get(), req_msg_ptr.get(), rsp_msg_ptr.get(), closure.get());
}

bool RpcDispatcher::parseServiceFullName(std::string_view full_name, std::string_view& service_name,
                                         std::string_view& method_name) {
    if (full_name.empty()) {
        ROCKET_LOG_ERROR("full name empty");
        return false;
    }
    size_t i = full_name.find_first_of('.');
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

void RpcDispatcher::stop() { m_worker_pool.stopAndDrain(); }

void RpcDispatcher::setTinyPBError(TinyPBProtocol::s_ptr msg, int32_t err_code, std::string_view err_info) {
    msg->m_err_code = err_code;
    msg->m_err_info = err_info;
    msg->m_err_info_len = static_cast<std::int32_t>(err_info.length());
}

} // namespace rocket
