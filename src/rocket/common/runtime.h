#pragma once
#include <cstdint>
#include <string>
namespace rocket {

class RpcInterface;

class RunTime {
  public:
    [[nodiscard]] RpcInterface* getRpcInterface() const;
    static RunTime* GetRunTime();

    std::uint64_t m_msgid{0};
    std::string m_method_name;
    RpcInterface* m_rpc_interface{nullptr};
};

} // namespace rocket
