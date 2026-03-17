#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace rocket {

/**
 * @brief 抽象协议基类
 *
 * 所有 RPC 协议（请求/响应）的基类，定义协议的公共属性。
 * 使用 enable_shared_from_this 支持从 this 获取 shared_ptr。
 *
 * 派生类应实现具体协议格式（如 TinyPB、HTTP 等）。
 */
struct AbstractProtocol : public std::enable_shared_from_this<AbstractProtocol> {
  public:
    using s_ptr = std::shared_ptr<AbstractProtocol>;

    AbstractProtocol() = default;
    virtual ~AbstractProtocol() = default;

    AbstractProtocol(const AbstractProtocol&) = default;
    AbstractProtocol& operator=(const AbstractProtocol&) = default;
    AbstractProtocol(AbstractProtocol&&) = default;
    AbstractProtocol& operator=(AbstractProtocol&&) = default;

    /**
     * @brief 获取协议类型标识
     * @return 协议类型字符串（如 "TinyPB", "HTTP"）
     */
    [[nodiscard]] virtual std::string_view getProtocolType() const = 0;

    std::string m_msg_id; ///< 消息唯一标识符（用于请求-响应匹配）
};

} // namespace rocket