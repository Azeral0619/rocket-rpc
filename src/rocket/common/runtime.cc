#include "rocket/common/runtime.h"

namespace rocket {

RpcInterface* RunTime::getRpcInterface() const noexcept {
    return m_rpc_interface;
}

} // namespace rocket
