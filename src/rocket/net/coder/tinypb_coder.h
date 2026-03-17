#pragma once

#include "rocket/net/coder/abstract_coder.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rocket {

/**
 * @brief TinyPB 协议编解码器
 *
 * 实现 TinyPB 协议的序列化和反序列化：
 * - encode: 将 TinyPBProtocol 对象编码为二进制流
 * - decode: 从二进制流解析 TinyPBProtocol 对象
 *
 * 特性：
 * - 支持粘包/拆包处理
 * - 网络字节序转换（大端）
 * - CRC32 校验和验证
 * - 边界安全检查
 *
 * 使用示例：
 * @code
 * auto coder = std::make_shared<TinyPBCoder>();
 * auto protocol = std::make_shared<TinyPBProtocol>();
 * protocol->m_method_name = "HelloService.SayHello";
 * protocol->m_pb_data = serialized_protobuf_data;
 *
 * std::vector<AbstractProtocol::s_ptr> messages = {protocol};
 * coder->encode(messages, tcp_buffer);
 * @endcode
 */
class TinyPBCoder : public AbstractCoder {
  public:
    using s_ptr = std::shared_ptr<TinyPBCoder>;
    TinyPBCoder() = default;
    ~TinyPBCoder() override = default;

    TinyPBCoder(const TinyPBCoder&) = delete;
    TinyPBCoder& operator=(const TinyPBCoder&) = delete;
    TinyPBCoder(TinyPBCoder&&) = delete;
    TinyPBCoder& operator=(TinyPBCoder&&) = delete;

    /**
     * @brief 编码：将 TinyPB 协议对象序列化为字节流
     *
     * @param messages 待编码的协议对象列表
     * @param out_buffer 输出缓冲区
     *
     * 编码流程：
     * 1. 写入起始标识 PB_START
     * 2. 写入总长度（网络字节序）
     * 3. 依次写入各字段（长度+内容）
     * 4. 计算并写入校验和
     * 5. 写入结束标识 PB_END
     */
    void encode(std::vector<AbstractProtocol::s_ptr>& messages, TcpBuffer::s_ptr out_buffer) override;

    /**
     * @brief 解码：从字节流解析 TinyPB 协议对象
     *
     * @param out_messages 输出的协议对象列表
     * @param buffer 输入缓冲区
     *
     * 解码流程：
     * 1. 查找起始标识 PB_START
     * 2. 读取总长度，验证结束标识 PB_END
     * 3. 依次解析各字段
     * 4. 验证校验和
     * 5. 移除已解析的数据
     *
     * 边界处理：
     * - 数据不完整：保留在缓冲区，等待更多数据
     * - 多个完整包：依次解析所有完整包
     * - 格式错误：跳过当前字节，继续查找下一个起始标识
     */
    void decode(std::vector<AbstractProtocol::s_ptr>& out_messages, TcpBuffer::s_ptr buffer) override;

  private:
    /**
     * @brief 将单个 TinyPB 协议对象编码为字节流
     *
     * @param message 待编码的协议对象
     * @return 编码后的字节流（使用 std::string 管理内存）
     */
    static std::string encodeTinyPB(const TinyPBProtocol::s_ptr& message);

    /**
     * @brief 计算 CRC32 校验和
     *
     * @param data 待计算的数据
     * @param len 数据长度
     * @return CRC32 校验和
     */
    static std::uint32_t calculateChecksum(const char* data, std::size_t len);
};

} // namespace rocket
