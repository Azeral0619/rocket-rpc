#pragma once

#include <cstdint>
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>
#include <string>
#include <string_view>

#include "rocket/common/log.h"
#include "rocket/net/tcp/net_addr.h"

namespace rocket {

class RpcController : public google::protobuf::RpcController {

  public:
    RpcController() { ROCKET_LOG_DEBUG("RpcController"); }
    ~RpcController() { ROCKET_LOG_DEBUG("~RpcController"); }

    RpcController(const RpcController&) = delete;
    RpcController& operator=(const RpcController&) = delete;
    RpcController(RpcController&&) = delete;
    RpcController& operator=(RpcController&&) = delete;

    void Reset() override;

    [[nodiscard]] bool Failed() const override;

    [[nodiscard]] std::string ErrorText() const override;

    void StartCancel() override;

    void SetFailed(const std::string& reason) override;

    [[nodiscard]] bool IsCanceled() const override;

    void NotifyOnCancel(google::protobuf::Closure* callback) override;

    void SetError(int32_t error_code, std::string_view error_info);

    [[nodiscard]] int32_t GetErrorCode() const;

    [[nodiscard]] std::string GetErrorInfo() const;

    void SetMsgId(std::string_view msg_id);

    std::string GetMsgId();

    void SetLocalAddr(NetAddr::s_ptr addr);

    void SetPeerAddr(NetAddr::s_ptr addr);

    [[nodiscard]] NetAddr::s_ptr GetLocalAddr() const;

    [[nodiscard]] NetAddr::s_ptr GetPeerAddr() const;

    void SetTimeout(int timeout);

    [[nodiscard]] int GetTimeout() const;

    [[nodiscard]] bool Finished() const;

    void SetFinished(bool value);

  private:
    static constexpr int kDefaultTimeoutMs = 1000;

    int32_t m_error_code{0};
    std::string m_error_info;
    std::string m_msg_id;

    bool m_is_failed{false};
    bool m_is_cancled{false};
    bool m_is_finished{false};

    NetAddr::s_ptr m_local_addr;
    NetAddr::s_ptr m_peer_addr;

    int m_timeout{kDefaultTimeoutMs}; // ms
};

} // namespace rocket