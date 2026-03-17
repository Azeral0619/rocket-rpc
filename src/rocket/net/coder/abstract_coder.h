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
class AbstractCoder {
  public:
    using s_ptr = std::shared_ptr<AbstractCoder>;

    AbstractCoder() = default;
    virtual ~AbstractCoder() = default;

    AbstractCoder(const AbstractCoder&) = delete;
    AbstractCoder& operator=(const AbstractCoder&) = delete;
    AbstractCoder(AbstractCoder&&) = delete;
    AbstractCoder& operator=(AbstractCoder&&) = delete;

    /**
     * @brief 编码：将协议对象序列化为字节流
     *
     * @param messages 待编码的协议对象列表（输入）
     * @param out_buffer 输出缓冲区（写入编码后的字节流）
     *
     * 工作流程：
     * 1. 遍历 messages 中的每个协议对象
     * 2. 按协议格式序列化（添加头部、长度、校验和等）
     * 3. 写入 out_buffer
     *
     * 注意：
     * - 支持批量编码（减少系统调用）
     * - 编码失败不应抛出异常，应在协议对象中标记错误
     */
    virtual void encode(std::vector<AbstractProtocol::s_ptr>& messages, TcpBuffer::s_ptr out_buffer) = 0;

    /**
     * @brief 解码：将字节流反序列化为协议对象
     *
     * @param out_messages 输出的协议对象列表（解码结果）
     * @param buffer 输入缓冲区（包含待解码的字节流）
     *
     * 工作流程：
     * 1. 从 buffer 读取数据
     * 2. 按协议格式解析（验证头部、长度、校验和等）
     * 3. 构造协议对象添加到 out_messages
     * 4. 从 buffer 移除已解析的数据
     *
     * 处理边界情况：
     * - 数据不完整：保留在 buffer 中，等待更多数据
     * - 多个完整消息：解析所有完整消息
     * - 格式错误：跳过错误数据或关闭连接
     *
     * 注意：
     * - 支持粘包/拆包处理
     * - 解码失败不应抛出异常
     */
    virtual void decode(std::vector<AbstractProtocol::s_ptr>& out_messages, TcpBuffer::s_ptr buffer) = 0;
};

} // namespace rocket