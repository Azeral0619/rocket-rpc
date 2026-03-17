#pragma once

#include "rocket/net/rpc/rpc_controller.h"
#include <functional>
#include <google/protobuf/message.h>
#include <memory>
#include <string_view>

namespace rocket {

class RpcClosure;

class RpcInterface : public std::enable_shared_from_this<RpcInterface> {
  public:

    RpcInterface(std::unique_ptr<const google::protobuf::Message> req, std::unique_ptr<google::protobuf::Message> rsp,
                 std::unique_ptr<RpcClosure> done, std::unique_ptr<RpcController> controller);

    virtual ~RpcInterface();

    RpcInterface(const RpcInterface&) = delete;
    RpcInterface& operator=(const RpcInterface&) = delete;
    RpcInterface(RpcInterface&&) = delete;
    RpcInterface& operator=(RpcInterface&&) = delete;

    void reply() noexcept;

    std::shared_ptr<RpcClosure> newRpcClosure(std::function<void()>& cb);

    virtual void run() = 0;

    virtual void setError(int code, std::string_view err_info) = 0;

  protected:
    std::unique_ptr<const google::protobuf::Message> m_req_base;
    std::unique_ptr<google::protobuf::Message> m_rsp_base;
    std::unique_ptr<RpcClosure> m_done;
    std::unique_ptr<RpcController> m_controller;
};

} // namespace rocket