#pragma once
#include <cstdint>
namespace rocket {

class MsgIDUtil {
  public:
    static std::uint64_t GenMsgID();
};

} // namespace rocket
