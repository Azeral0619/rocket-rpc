#include "rocket/common/runtime.h"

namespace rocket {

thread_local RunTime* t_run_time = nullptr;

RunTime* RunTime::GetRunTime() {
    if (t_run_time != nullptr) {
        return t_run_time;
    }
    t_run_time = new RunTime();
    return t_run_time;
}

RpcInterface* RunTime::getRpcInterface() const { return m_rpc_interface; }

} // namespace rocket