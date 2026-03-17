#include "rocket/net/rpc/rpc_channel.h"

#include "rocket/common/config.h"
#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/msg_id_util.h"
#include "rocket/common/runtime.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_client.h"
#include "rocket/net/timer_event.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rocket {

RpcChannel::RpcChannel(NetAddr::s_ptr peer_addr) : m_peer_addr(std::move(peer_addr)) { ROCKET_LOG_DEBUG("RpcChannel"); }

RpcChannel::~RpcChannel() { ROCKET_LOG_DEBUG("~RpcChannel"); }

void RpcChannel::callBack() {
    auto* my_controller = dynamic_cast<RpcController*>(getController());
    if (my_controller->Finished()) {
        return;
    }

    if (m_closure) {
        m_closure->Run();
        if (my_controller != nullptr) {
            my_controller->SetFinished(true);
        }
    }
}

void RpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                            google::protobuf::RpcController* controller, const google::protobuf::Message* request,
                            google::protobuf::Message* response, google::protobuf::Closure* done) {

    std::shared_ptr<rocket::TinyPBProtocol> req_protocol = std::make_shared<rocket::TinyPBProtocol>();

    auto* my_controller = dynamic_cast<RpcController*>(controller);
    if (my_controller == nullptr || request == nullptr || response == nullptr) {
        ROCKET_LOG_ERROR("failed callmethod, RpcController convert error");
        if (my_controller != nullptr) {
            my_controller->SetError(error::kRpcChannelInit, "controller or request or response NULL");
        }
        callBack();
        return;
    }

    if (m_peer_addr == nullptr) {
        ROCKET_LOG_ERROR("failed get peer addr");
        my_controller->SetError(error::kRpcPeerAddr, "peer addr nullptr");
        callBack();
        return;
    }

    m_client = std::make_shared<TcpClient>(m_peer_addr);

    if (my_controller->GetMsgId().empty()) {
        // 先从 runtime 里面取, 取不到再生成一个
        // 这样的目的是为了实现 msg_id 的透传，假设服务 A 调用了 B，那么同一个 msgid 可以在服务 A 和 B
        // 之间串起来，方便日志追踪
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
        callBack();
        return;
    }

    if (!request->SerializeToString(&(req_protocol->m_pb_data))) {
        std::string err_info = "failde to serialize";
        my_controller->SetError(error::kFailedSerialize, err_info);
        ROCKET_LOG_ERROR("{} | {}, origin requeset [{}] ", req_protocol->m_msg_id, err_info,
                         request->ShortDebugString());
        callBack();
        return;
    }

    auto channel = shared_from_this();

    auto timer_event =
        std::make_shared<TimerEvent>(my_controller->GetTimeout(), false, [my_controller, channel]() mutable {
            ROCKET_LOG_INFO("{} | call rpc timeout arrive", my_controller->GetMsgId());
            if (my_controller->Finished()) {
                channel.reset();
                return;
            }

            my_controller->StartCancel();
            my_controller->SetError(error::kRpcCallTimeout,
                                    "rpc call timeout " + std::to_string(my_controller->GetTimeout()));

            channel->callBack();
            channel.reset();
        });

    m_client->addTimerEvent(timer_event);

    m_client->connect([req_protocol, this]() mutable {
        auto* my_controller = dynamic_cast<RpcController*>(getController());

        if (getTcpClient()->getConnectErrorCode() != 0) {
            my_controller->SetError(getTcpClient()->getConnectErrorCode(), getTcpClient()->getConnectErrorInfo());
            ROCKET_LOG_ERROR("{} | connect error, error code[{}], error info[{}], peer addr[{}]",
                             req_protocol->m_msg_id, my_controller->GetErrorCode(), my_controller->GetErrorInfo(),
                             getTcpClient()->getPeerAddr()->toString());

            callBack();

            return;
        }

        ROCKET_LOG_INFO("{} | connect success, peer addr[{}], local addr[{}]", req_protocol->m_msg_id,
                        getTcpClient()->getPeerAddr()->toString(), getTcpClient()->getLocalAddr()->toString());

        getTcpClient()->writeMessage(req_protocol, [req_protocol, this,
                                                    my_controller](const std::shared_ptr<AbstractProtocol>&) mutable {
            ROCKET_LOG_INFO("{} | send rpc request success. call method name[{}], peer addr[{}], local addr[{}]",
                            req_protocol->m_msg_id, req_protocol->m_method_name,
                            getTcpClient()->getPeerAddr()->toString(), getTcpClient()->getLocalAddr()->toString());

            getTcpClient()->readMessage(
                req_protocol->m_msg_id, [this, my_controller](const std::shared_ptr<AbstractProtocol>& msg) mutable {
                    std::shared_ptr<rocket::TinyPBProtocol> rsp_protocol =
                        std::dynamic_pointer_cast<rocket::TinyPBProtocol>(msg);
                    ROCKET_LOG_INFO(
                        "{} | success get rpc response, call method name[{}], peer addr[{}], local addr[{}]",
                        rsp_protocol->m_msg_id, rsp_protocol->m_method_name, getTcpClient()->getPeerAddr()->toString(),
                        getTcpClient()->getLocalAddr()->toString());

                    if (!(getResponse()->ParseFromString(rsp_protocol->m_pb_data))) {
                        ROCKET_LOG_ERROR("{} | serialize error", rsp_protocol->m_msg_id);
                        my_controller->SetError(error::kFailedSerialize, "serialize error");
                        callBack();
                        return;
                    }

                    if (rsp_protocol->m_err_code != 0) {
                        ROCKET_LOG_ERROR("{} | call rpc methood[{}] failed, error code[{}], error info[{}]",
                                         rsp_protocol->m_msg_id, rsp_protocol->m_method_name, rsp_protocol->m_err_code,
                                         rsp_protocol->m_err_info);

                        my_controller->SetError(rsp_protocol->m_err_code, rsp_protocol->m_err_info);
                        callBack();
                        return;
                    }

                    ROCKET_LOG_INFO("{} | call rpc success, call method name[{}], peer addr[{}], local addr[{}]",
                                    rsp_protocol->m_msg_id, rsp_protocol->m_method_name,
                                    getTcpClient()->getPeerAddr()->toString(),
                                    getTcpClient()->getLocalAddr()->toString());

                    callBack();
                });
        });
    });
}

void RpcChannel::Init(controller_s_ptr controller, message_s_ptr req, message_s_ptr res, closure_s_ptr done) {
    if (m_is_init) {
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
    if (IPNetAddr::checkValid(str)) {
        return std::make_shared<IPNetAddr>(str);
    }

    const auto config = Config::getInstance().getConfig();
    auto it = config->rpc_stubs.find(str);
    if (it != config->rpc_stubs.end()) {
        ROCKET_LOG_INFO("find addr [{}] in global config of str[{}]", it->second.addr->toString(), str);
        return it->second.addr;
    }

    ROCKET_LOG_INFO("can not find addr in global config of str[{}]", str);
    return nullptr;
}

} // namespace rocket