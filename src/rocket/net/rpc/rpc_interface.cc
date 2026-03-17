#include "rocket/net/rpc/rpc_interface.h"
#include "rocket/common/log.h"
#include "rocket/net/rpc/rpc_closure.h"
#include "rocket/net/rpc/rpc_controller.h"
#include <functional>
#include <google/protobuf/message.h>
#include <memory>
#include <utility>

namespace rocket {

RpcInterface::RpcInterface(std::unique_ptr<const google::protobuf::Message> req,
                           std::unique_ptr<google::protobuf::Message> rsp, std::unique_ptr<RpcClosure> done,
                           std::unique_ptr<RpcController> controller)
    : m_req_base(std::move(req)), m_rsp_base(std::move(rsp)), m_done(std::move(done)),
      m_controller(std::move(controller)) {
    ROCKET_LOG_DEBUG("RpcInterface");
}

RpcInterface::~RpcInterface() {
    ROCKET_LOG_DEBUG("~RpcInterface");
    reply();
}

void RpcInterface::reply() noexcept {
    if (m_done) {
        m_done->Run();
    }
}

std::shared_ptr<RpcClosure> RpcInterface::newRpcClosure(std::function<void()>& cb) {
    return std::make_shared<RpcClosure>(shared_from_this(), cb);
}

} // namespace rocket