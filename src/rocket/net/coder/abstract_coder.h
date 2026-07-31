#pragma once

#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <memory>
#include <vector>

namespace rocket {

/**
 * @brief 抽象编解码器接口
 *
 * 定义协议编解码器的统一接口，负责：
 * 1. 序列化：将协议对象编码为字节流
 * 2. 反序列化：将字节流解码为协议对象
 *
 * 设计模式：策略模式
 * - 不同协议实现不同的编解码策略（TinyPBCoder, HttpCoder）
 * - 运行时可替换编解码器
 *
 * 典型实现：
 * - TinyPBCoder: 处理 TinyPB 协议（自定义二进制格式）
 * - JsonCoder: 处理 JSON 协议
 * - ProtobufCoder: 处理 Protobuf 协议
 */
struct DecodeResult {
    std::vector<AbstractProtocol::s_ptr> messages;
    bool fatal{false};
};

class AbstractCoder {
  public:
    using s_ptr = std::shared_ptr<AbstractCoder>;

    AbstractCoder() = default;
    virtual ~AbstractCoder() = default;

    AbstractCoder(const AbstractCoder&) = delete;
    AbstractCoder& operator=(const AbstractCoder&) = delete;
    AbstractCoder(AbstractCoder&&) = delete;
    AbstractCoder& operator=(AbstractCoder&&) = delete;

    [[nodiscard]] virtual bool encode(std::vector<AbstractProtocol::s_ptr>& messages,
                                      TcpBuffer::s_ptr out_buffer) = 0;

    virtual DecodeResult decode(TcpBuffer::s_ptr buffer) = 0;
};

} // namespace rocket
