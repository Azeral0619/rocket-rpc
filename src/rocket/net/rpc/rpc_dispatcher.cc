#include <cstddef>
#include <cstdint>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/runtime.h"
#include "rocket/common/thread_pool.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/rpc/rpc_closure.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/rpc/rpc_dispatcher.h"
#include "rocket/net/tcp/tcp_connect.h"

namespace rocket {

void RpcDispatcher::dispatch(AbstractProtocol::s_ptr request, AbstractProtocol::s_ptr response,
                             TcpConnection* connection) {

    auto req_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(request);
    auto rsp_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(response);

    std::string method_full_name = req_protocol->m_method_name;
    std::string_view service_name;
    std::string_view method_name;

    rsp_protocol->m_msg_id = req_protocol->m_msg_id;
    rsp_protocol->m_method_name = req_protocol->m_method_name;

    if (!parseServiceFullName(method_full_name, service_name, method_name)) {
        setTinyPBError(rsp_protocol, error::kParseServiceName, "parse service name error");
        return;
    }

    auto it = m_service_map.find(service_name);
    if (it == m_service_map.end()) {
        ROCKET_LOG_ERROR("{} | service name[{}] not found", req_protocol->m_msg_id, service_name);
        setTinyPBError(rsp_protocol, error::kServiceNotFound, "service not found");
        return;
    }

    const auto& service = it->second;

    const auto* method = service->GetDescriptor()->FindMethodByName(method_name);
    if (method == nullptr) {
        ROCKET_LOG_ERROR("{} | method name[{}] not found in service[{}]", req_protocol->m_msg_id, method_name,
                         service_name);
        setTinyPBError(rsp_protocol, error::kMethodNotFound, "method not found");
        return;
    }

    std::unique_ptr<google::protobuf::Message> req_msg(service->GetRequestPrototype(method).New());

    if (!req_msg->ParseFromString(req_protocol->m_pb_data)) {
        ROCKET_LOG_ERROR("{} | deserialize error", req_protocol->m_msg_id);
        setTinyPBError(rsp_protocol, error::kFailedDeserialize, "deserialize error");
        return;
    }

    ROCKET_LOG_INFO("{} | get rpc request[{}]", req_protocol->m_msg_id, req_msg->ShortDebugString());

    std::unique_ptr<google::protobuf::Message> rsp_msg(service->GetResponsePrototype(method).New());

    auto rpc_controller = std::make_unique<RpcController>();
    rpc_controller->SetLocalAddr(connection->getLocalAddr());
    rpc_controller->SetPeerAddr(connection->getPeerAddr());
    rpc_controller->SetMsgId(req_protocol->m_msg_id);

    RunTime::GetRunTime()->m_msgid = req_protocol->m_msg_id;
    RunTime::GetRunTime()->m_method_name = method_name;

    auto req_msg_ptr = std::shared_ptr<google::protobuf::Message>(req_msg.release());
    auto rsp_msg_ptr = std::shared_ptr<google::protobuf::Message>(rsp_msg.release());
    auto controller_ptr = std::shared_ptr<RpcController>(rpc_controller.release());

    auto closure = std::make_shared<RpcClosure>(
        nullptr, [req_msg_ptr, rsp_msg_ptr, req_protocol, rsp_protocol, connection, controller_ptr, this]() mutable {
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
            std::vector<AbstractProtocol::s_ptr> replay_messages;
            replay_messages.emplace_back(rsp_protocol);
            connection->reply(replay_messages);
        });

    m_worker_pool.execute([service, method, controller_ptr, req_msg_ptr, rsp_msg_ptr, closure]() mutable {
        service->CallMethod(method, controller_ptr.get(), req_msg_ptr.get(), rsp_msg_ptr.get(), closure.get());
    });
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

    ROCKET_LOG_INFO("parse service_name[{}] and method_name[{}] from full name [{}]", service_name, method_name,
                    full_name);

    return true;
}

void RpcDispatcher::registerService(Services_ptr service) {
    std::string service_name(service->GetDescriptor()->full_name());
    m_service_map[service_name] = service;
}

void RpcDispatcher::setTinyPBError(TinyPBProtocol::s_ptr msg, int32_t err_code, std::string_view err_info) {
    msg->m_err_code = err_code;
    msg->m_err_info = err_info;
    msg->m_err_info_len = static_cast<std::int32_t>(err_info.length());
}

} // namespace rocket