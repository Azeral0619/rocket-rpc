#pragma once

#include <cstdint>

namespace rocket {
namespace error {

// 错误码前缀宏
#ifndef SYS_ERROR_PREFIX
#define SYS_ERROR_PREFIX(xx) 1000##xx
#endif

// 系统错误码定义
inline constexpr std::int32_t kPeerClosed = SYS_ERROR_PREFIX(0000);        // 连接时对端关闭
inline constexpr std::int32_t kFailedConnect = SYS_ERROR_PREFIX(0001);     // 连接失败
inline constexpr std::int32_t kFailedGetReply = SYS_ERROR_PREFIX(0002);    // 获取回包失败
inline constexpr std::int32_t kFailedDeserialize = SYS_ERROR_PREFIX(0003); // 反序列化失败
inline constexpr std::int32_t kFailedSerialize = SYS_ERROR_PREFIX(0004);   // 序列化失败

inline constexpr std::int32_t kFailedEncode = SYS_ERROR_PREFIX(0005); // 编码失败
inline constexpr std::int32_t kFailedDecode = SYS_ERROR_PREFIX(0006); // 解码失败

inline constexpr std::int32_t kRpcCallTimeout = SYS_ERROR_PREFIX(0007); // RPC 调用超时

inline constexpr std::int32_t kServiceNotFound = SYS_ERROR_PREFIX(0008);  // 服务不存在
inline constexpr std::int32_t kMethodNotFound = SYS_ERROR_PREFIX(0009);   // 方法不存在
inline constexpr std::int32_t kParseServiceName = SYS_ERROR_PREFIX(0010); // 服务名解析失败
inline constexpr std::int32_t kRpcChannelInit = SYS_ERROR_PREFIX(0011);   // RPC channel 初始化失败
inline constexpr std::int32_t kRpcPeerAddr = SYS_ERROR_PREFIX(0012);      // RPC 调用对端地址异常

} // namespace error
} // namespace rocket