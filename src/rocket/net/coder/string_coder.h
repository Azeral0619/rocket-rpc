#pragma once

#include "rocket/net/coder/abstract_coder.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rocket {

/**
 * @brief 字符串协议
 *
 * 最简单的协议实现，直接传输原始字符串，不添加任何协议头。
 * 适用于：
 * - 测试和调试
 * - 简单的文本传输场景
 * - Echo 服务器
 *
 * 注意：无法处理粘包/拆包问题，实际生产应使用带长度字段的协议。
 */
class StringProtocol : public AbstractProtocol {
  public:
    using s_ptr = std::shared_ptr<StringProtocol>;

    StringProtocol() = default;
    ~StringProtocol() override = default;

    StringProtocol(const StringProtocol&) = default;
    StringProtocol& operator=(const StringProtocol&) = default;
    StringProtocol(StringProtocol&&) = default;
    StringProtocol& operator=(StringProtocol&&) = default;

    [[nodiscard]] std::string_view getProtocolType() const override { return "String"; }
    [[nodiscard]] std::size_t estimatedWireSize() const noexcept override {
        return info.size();
    }

    std::string info; ///< 消息内容
};

/**
 * @brief 字符串编解码器
 *
 * 简单的字符串协议编解码实现：
 * - encode: 直接写入原始字符串
 * - decode: 读取所有可用数据作为一个字符串消息
 *
 * 限制：
 * - 无消息边界，无法区分多个消息
 * - 适合请求-响应模式（一次完整的读写）
 * - 不适合流式传输
 */
class StringCoder : public AbstractCoder {
  public:
    StringCoder() = default;
    ~StringCoder() override = default;

    StringCoder(const StringCoder&) = delete;
    StringCoder& operator=(const StringCoder&) = delete;
    StringCoder(StringCoder&&) = delete;
    StringCoder& operator=(StringCoder&&) = delete;

    /**
     * @brief 编码：将字符串协议对象写入缓冲区
     *
     * @param messages 待编码的消息列表
     * @param out_buffer 输出缓冲区
     *
     * 实现：遍历所有消息，依次写入它们的 info 字段
     */
    using AbstractCoder::decode;
    using AbstractCoder::encode;

    [[nodiscard]] bool encode(
        std::span<const AbstractProtocol::s_ptr> messages,
        TcpBuffer& out_buffer) override {
        for (const auto& msg_base : messages) {
            auto* msg = dynamic_cast<StringProtocol*>(msg_base.get());
            if (msg == nullptr || msg->info.empty()) {
                continue;
            }
            if (!out_buffer.writeToBuffer(msg->info)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 解码：将缓冲区中的所有数据作为一个字符串消息
     *
     * @param out_messages 输出消息列表
     * @param buffer 输入缓冲区
     *
     * 实现：读取缓冲区中的所有可读数据，构造一个 StringProtocol 对象
     *
     * 注意：这会清空缓冲区的所有数据
     */
    bool decode(TcpBuffer& buffer,
                std::vector<AbstractProtocol::s_ptr>& output) override {
        const std::size_t readable = buffer.readAble();
        if (readable == 0) {
            return true;
        }

        auto msg = std::make_shared<StringProtocol>();
        msg->info.assign(buffer.readableView());
        msg->m_msg_id = 1;
        buffer.consumeAll();

        output.push_back(std::move(msg));
        return true;
    }
};

} // namespace rocket
