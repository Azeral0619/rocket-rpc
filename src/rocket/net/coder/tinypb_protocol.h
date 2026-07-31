#pragma once

#include "rocket/net/coder/abstract_protocol.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rocket {

/**
 * @brief TinyPB 协议格式
 *
 * TinyPB（Tiny Protocol Buffer）是一个轻量级的二进制 RPC 协议。
 *
 * 协议结构（所有多字节整数使用网络字节序）：
 * +-------+--------+-------------+------------------+--------+--------------+
 * | Start | PkLen  | MsgId       | MethodNameLen    | Method | ErrCode      |
 * | 1B    | 4B     | 8B          | 4B               | var    | 4B           |
 * +-------+--------+-------------+------------------+--------+--------------+
 * | ErrInfoLen | ErrInfo | PbData      | CheckSum | End  |
 * | 4B         | var     | var         | 4B       | 1B   |
 * +------------+---------+-------------+----------+------+
 *
 * 字段说明：
 * - Start: 起始标识符 0x02
 * - PkLen: 整包长度（包括起始和结束标识符）
 * - MsgId: 固定 64 位无符号消息ID（用于请求-响应匹配）
 * - MethodNameLen: 方法名长度
 * - MethodName: RPC 方法名
 * - ErrCode: 错误码（0表示成功）
 * - ErrInfoLen: 错误信息长度
 * - ErrInfo: 错误描述
 * - PbData: Protobuf 序列化的数据
 * - CheckSum: 校验和（CRC32）
 * - End: 结束标识符 0x03
 *
 * 固定开销：2（起始+结束） + 20（5个int32）+ 8（MsgId）= 30 字节
 */
struct TinyPBProtocol : public AbstractProtocol {
  public:
    using s_ptr = std::shared_ptr<TinyPBProtocol>;

    static constexpr char PB_START = 0x02; ///< 起始标识
    static constexpr char PB_END = 0x03;   ///< 结束标识

    static constexpr std::size_t HEADER_SIZE =
        1 + sizeof(std::int32_t) + sizeof(MessageId) +
        (sizeof(std::int32_t) * 4) + 1;

    TinyPBProtocol() = default;
    ~TinyPBProtocol() override = default;

    TinyPBProtocol(const TinyPBProtocol&) = default;
    TinyPBProtocol& operator=(const TinyPBProtocol&) = default;
    TinyPBProtocol(TinyPBProtocol&&) = default;
    TinyPBProtocol& operator=(TinyPBProtocol&&) = default;

    [[nodiscard]] std::string_view getProtocolType() const override { return "TinyPB"; }

    // 协议字段
    std::int32_t m_pk_len{0}; ///< 整包长度
    // m_msg_id 继承自 AbstractProtocol

    std::int32_t m_method_name_len{0}; ///< 方法名长度
    std::string m_method_name;         ///< RPC 方法名

    std::int32_t m_err_code{0};     ///< 错误码（0=成功）
    std::int32_t m_err_info_len{0}; ///< 错误信息长度
    std::string m_err_info;         ///< 错误描述

    std::string m_pb_data;       ///< Protobuf 数据
    std::int32_t m_check_sum{0}; ///< 校验和

    bool parse_success{false}; ///< 解析是否成功
};

} // namespace rocket
