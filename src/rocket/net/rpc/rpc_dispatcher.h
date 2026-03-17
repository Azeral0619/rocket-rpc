#pragma once

#include <cstdint>
#include <functional>
#include <google/protobuf/service.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "rocket/common/singleton.h"
#include "rocket/common/thread_pool.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"

namespace rocket {

class TcpConnection;

class RpcDispatcher : public Singleton<RpcDispatcher> {
  public:
    using Services_ptr = std::shared_ptr<google::protobuf::Service>;

    void dispatch(AbstractProtocol::s_ptr request, AbstractProtocol::s_ptr response, TcpConnection* connection);

    void registerService(Services_ptr service);

    static void setTinyPBError(TinyPBProtocol::s_ptr msg, int32_t err_code, std::string_view err_info);

  private:

    static bool parseServiceFullName(std::string_view full_name, std::string_view& service_name,
                                     std::string_view& method_name);

    std::map<std::string, Services_ptr, std::less<>> m_service_map;
    ThreadPool m_worker_pool{ThreadPool::kDefaultThreadCount};
};

} // namespace rocket