#include "rocket/common/runtime.h"

namespace rocket {

RunTime* RunTime::GetRunTime() {
    static thread_local RunTime runtime;
    return &runtime;
}

RpcInterface* RunTime::getRpcInterface() const { return m_rpc_interface; }

} // namespace rocket
