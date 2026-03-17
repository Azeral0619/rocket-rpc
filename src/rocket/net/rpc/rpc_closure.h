#pragma once

#include "rocket/common/exception.h"
#include "rocket/common/log.h"
#include "rocket/common/runtime.h"
#include "rocket/net/rpc/rpc_interface.h"
#include <exception>
#include <functional>
#include <google/protobuf/stubs/callback.h>
#include <memory>
#include <string>
#include <utility>

namespace rocket {

class RpcClosure : public google::protobuf::Closure {
  public:
    using Interfaces_ptr = std::shared_ptr<RpcInterface>;

    RpcClosure(Interfaces_ptr interface, std::function<void()> cb)
        : m_rpc_interface(std::move(interface)), m_cb(std::move(cb)) {
        ROCKET_LOG_DEBUG("RpcClosure");
    }

    ~RpcClosure() override { ROCKET_LOG_DEBUG("~RpcClosure"); }

    RpcClosure(const RpcClosure&) = delete;
    RpcClosure& operator=(const RpcClosure&) = delete;
    RpcClosure(RpcClosure&&) = delete;
    RpcClosure& operator=(RpcClosure&&) = delete;

    void Run() override {
        if (m_rpc_interface) {
            RunTime::GetRunTime()->m_rpc_interface = m_rpc_interface.get();
        }

        try {
            if (m_cb != nullptr) {
                m_cb();
            }
            if (m_rpc_interface) {
                m_rpc_interface.reset();
            }
        } catch (RocketException& e) {
            ROCKET_LOG_ERROR("RocketException exception[{}], deal handle", e.what());
            e.handle();
            if (m_rpc_interface) {
                m_rpc_interface->setError(e.errorCode(), std::string(e.errorInfo()));
                m_rpc_interface.reset();
            }
        } catch (std::exception& e) {
            ROCKET_LOG_ERROR("std::exception[{}]", e.what());
            if (m_rpc_interface) {
                m_rpc_interface->setError(-1, "unknown std::exception");
                m_rpc_interface.reset();
            }
        } catch (...) {
            ROCKET_LOG_ERROR("Unknown exception");
            if (m_rpc_interface) {
                m_rpc_interface->setError(-1, "unknown exception");
                m_rpc_interface.reset();
            }
        }
    }

  private:
    Interfaces_ptr m_rpc_interface;
    std::function<void()> m_cb;
};

} // namespace rocket