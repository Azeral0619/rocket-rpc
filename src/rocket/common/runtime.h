#pragma once
#include <string>
namespace rocket {

class RpcInterface;

class RunTime {
  public:
    [[nodiscard]] RpcInterface* getRpcInterface() const;
    static RunTime* GetRunTime();

    std::string m_msgid;
    std::string m_method_name;
    RpcInterface* m_rpc_interface{nullptr};
};

} // namespace rocket
