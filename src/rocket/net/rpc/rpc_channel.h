#pragma once

#include "rocket/net/tcp/net_addr.h"
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>
#include <memory>
#include <string_view>

namespace rocket {

// Forward declarations
class TcpClient;
class RpcController;
class RpcChannel;

// Type-safe helper functions to replace macros
template <typename MessageType>
inline std::shared_ptr<MessageType> NewMessage() {
    return std::make_shared<MessageType>();
}

inline std::shared_ptr<RpcController> NewRpcController();

inline std::shared_ptr<RpcChannel> NewRpcChannel(std::string_view addr);

class RpcChannel : public google::protobuf::RpcChannel, public std::enable_shared_from_this<RpcChannel> {

  public:
    using s_ptr = std::shared_ptr<RpcChannel>;
    using controller_s_ptr = std::shared_ptr<google::protobuf::RpcController>;
    using message_s_ptr = std::shared_ptr<google::protobuf::Message>;
    using closure_s_ptr = std::shared_ptr<google::protobuf::Closure>;

    [[nodiscard]] static NetAddr::s_ptr FindAddr(std::string_view str);

    explicit RpcChannel(NetAddr::s_ptr peer_addr);

    ~RpcChannel() override;

    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;
    RpcChannel(RpcChannel&&) = delete;
    RpcChannel& operator=(RpcChannel&&) = delete;

    void Init(controller_s_ptr controller, message_s_ptr req, message_s_ptr res, closure_s_ptr done);

    void CallMethod(const google::protobuf::MethodDescriptor* method, google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request, google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

    google::protobuf::RpcController* getController();

    google::protobuf::Message* getRequest();

    google::protobuf::Message* getResponse();

    google::protobuf::Closure* getClosure();

    TcpClient* getTcpClient();

  private:
    void callBack();

    std::shared_ptr<NetAddr> m_peer_addr{nullptr};
    std::shared_ptr<NetAddr> m_local_addr{nullptr};

    controller_s_ptr m_controller{nullptr};
    message_s_ptr m_request{nullptr};
    message_s_ptr m_response{nullptr};
    closure_s_ptr m_closure{nullptr};

    bool m_is_init{false};

    std::shared_ptr<TcpClient> m_client{nullptr};
};

inline std::shared_ptr<RpcController> NewRpcController() { return std::make_shared<RpcController>(); }

inline std::shared_ptr<RpcChannel> NewRpcChannel(std::string_view addr) {
    return std::make_shared<RpcChannel>(RpcChannel::FindAddr(addr));
}

} // namespace rocket
