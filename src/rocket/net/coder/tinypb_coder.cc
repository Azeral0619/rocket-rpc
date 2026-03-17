#include "rocket/net/coder/tinypb_coder.h"
#include "3rd-party/CRC.h"
#include "rocket/common/log.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

namespace rocket {

namespace {

constexpr std::size_t kMaxPacketSize = 1024ULL * 1024;

inline void writeInt32(std::string& buffer, std::int32_t value) {
    const std::uint32_t net_value = htonl(static_cast<std::uint32_t>(value));
    const auto* bytes = static_cast<const void*>(&net_value);
    buffer.append(static_cast<const char*>(bytes), sizeof(net_value));
}

inline std::int32_t readInt32(const char* data) {
    std::uint32_t net_value = 0;
    std::memcpy(&net_value, data, sizeof(net_value));
    return static_cast<std::int32_t>(ntohl(net_value));
}

} // anonymous namespace

void TinyPBCoder::encode(std::vector<AbstractProtocol::s_ptr>& messages, TcpBuffer::s_ptr out_buffer) {
    for (auto& msg_base : messages) {
        auto msg = std::dynamic_pointer_cast<TinyPBProtocol>(msg_base);
        if (!msg) {
            ROCKET_LOG_WARN("Skip non-TinyPBProtocol message in encode");
            continue;
        }

        std::string encoded = encodeTinyPB(msg);
        if (!encoded.empty()) {
            out_buffer->writeToBuffer(encoded.c_str(), encoded.length());
            ROCKET_LOG_DEBUG("Encoded TinyPB message [{}], {} bytes", msg->m_msg_id, encoded.length());
        }
    }
}

void TinyPBCoder::decode(std::vector<AbstractProtocol::s_ptr>& out_messages, TcpBuffer::s_ptr buffer) {
    while (true) {
        const std::size_t readable = buffer->readAble();
        if (readable == 0) {
            return;
        }

        std::vector<char> tmp;
        tmp.resize(readable);
        std::memcpy(tmp.data(), buffer->readableSpan().data(), readable);

        const std::size_t start_index = 0;
        const std::size_t write_index = readable;

        ROCKET_LOG_DEBUG("Decode: readable={} bytes, first byte=0x{:02x}", readable,
                         static_cast<unsigned char>(tmp[0]));

        std::size_t pk_start_index = 0;
        std::size_t pk_end_index = 0;
        std::int32_t pk_len = 0;
        bool found_valid_packet = false;

        for (std::size_t i = start_index; i < write_index; ++i) {
            if (tmp[i] == TinyPBProtocol::PB_START) {
                if (i + 1 + sizeof(std::int32_t) > write_index) {
                    return;
                }

                pk_len = readInt32(&tmp[i + 1]);
                ROCKET_LOG_DEBUG("Found PB_START at {}, pk_len={}", i, pk_len);

                if (pk_len <= 0 || static_cast<std::size_t>(pk_len) > kMaxPacketSize) {
                    ROCKET_LOG_WARN("Invalid pk_len={}, skip byte", pk_len);
                    buffer->moveReadIndex(1);
                    break; // 重新读取 buffer
                }

                const std::size_t pk_end_pos = i + static_cast<std::size_t>(pk_len) - 1;
                if (pk_end_pos >= write_index) {
                    ROCKET_LOG_DEBUG("Incomplete packet, need {} bytes", pk_len);
                    return;
                }

                if (tmp[pk_end_pos] == TinyPBProtocol::PB_END) {
                    pk_start_index = i;
                    pk_end_index = pk_end_pos;
                    found_valid_packet = true;
                    ROCKET_LOG_DEBUG("Found valid packet: start={}, end={}, len={}", pk_start_index, pk_end_index,
                                     pk_len);
                    break;
                }

                ROCKET_LOG_WARN("PB_END not found at expected position {}, byte=0x{:02x}, skip byte", pk_end_pos,
                                static_cast<unsigned char>(tmp[pk_end_pos]));
                buffer->moveReadIndex(1);
                break; // 重新读取 buffer
            }
        }

        if (!found_valid_packet) {
            ROCKET_LOG_DEBUG("No valid packet found, waiting");
            return;
        }

        auto message = std::make_shared<TinyPBProtocol>();
        message->m_pk_len = pk_len;

        std::size_t parse_index = pk_start_index + 1 + sizeof(std::int32_t);

        auto checkRemaining = [&](std::size_t need_bytes) -> bool {
            if (parse_index + need_bytes > pk_end_index) {
                ROCKET_LOG_ERROR("Parse error at index {}, need {} bytes", parse_index, need_bytes);
                message->parse_success = false;
                buffer->moveReadIndex(pk_end_index - pk_start_index + 1);
                return false;
            }
            return true;
        };

        // 1. msg_id
        if (!checkRemaining(sizeof(std::int32_t))) {
            continue;
        }
        message->m_msg_id_len = readInt32(&tmp[parse_index]);
        parse_index += sizeof(std::int32_t);

        if (!checkRemaining(message->m_msg_id_len)) {
            continue;
        }
        message->m_msg_id.assign(&tmp[parse_index], message->m_msg_id_len);
        parse_index += message->m_msg_id_len;
        ROCKET_LOG_DEBUG("msg_id: {}", message->m_msg_id);

        // 2. method_name
        if (!checkRemaining(sizeof(std::int32_t))) {
            continue;
        }
        message->m_method_name_len = readInt32(&tmp[parse_index]);
        parse_index += sizeof(std::int32_t);

        if (!checkRemaining(message->m_method_name_len)) {
            continue;
        }
        message->m_method_name.assign(&tmp[parse_index], message->m_method_name_len);
        parse_index += message->m_method_name_len;
        ROCKET_LOG_DEBUG("method: {}", message->m_method_name);

        // 3. err_code
        if (!checkRemaining(sizeof(std::int32_t))) {
            continue;
        }
        message->m_err_code = readInt32(&tmp[parse_index]);
        parse_index += sizeof(std::int32_t);

        // 4. err_info
        if (!checkRemaining(sizeof(std::int32_t))) {
            continue;
        }
        message->m_err_info_len = readInt32(&tmp[parse_index]);
        parse_index += sizeof(std::int32_t);

        if (!checkRemaining(message->m_err_info_len)) {
            continue;
        }
        message->m_err_info.assign(&tmp[parse_index], message->m_err_info_len);
        parse_index += message->m_err_info_len;

        // 5. pb_data
        const std::int32_t pb_data_len = pk_len - static_cast<std::int32_t>(TinyPBProtocol::HEADER_SIZE) -
                                         message->m_msg_id_len - message->m_method_name_len - message->m_err_info_len;

        if (pb_data_len < 0) {
            ROCKET_LOG_ERROR("Invalid pb_data_len={}", pb_data_len);
            message->parse_success = false;
            buffer->moveReadIndex(pk_end_index - pk_start_index + 1);
            continue;
        }

        if (!checkRemaining(pb_data_len)) {
            continue;
        }
        message->m_pb_data.assign(&tmp[parse_index], pb_data_len);
        parse_index += pb_data_len;

        // 6. checksum
        if (!checkRemaining(sizeof(std::int32_t))) {
            continue;
        }
        message->m_check_sum = readInt32(&tmp[parse_index]);

        buffer->moveReadIndex(pk_end_index - pk_start_index + 1);
        message->parse_success = true;
        out_messages.push_back(message);

        ROCKET_LOG_INFO("Decoded message [{}], method: {}", message->m_msg_id, message->m_method_name);
    }
}

std::string TinyPBCoder::encodeTinyPB(const TinyPBProtocol::s_ptr& message) {
    if (!message) {
        return "";
    }

    if (message->m_msg_id.empty()) {
        message->m_msg_id = "default_msg_id";
    }

    const auto pk_len = static_cast<std::int32_t>(TinyPBProtocol::HEADER_SIZE + message->m_msg_id.length() +
                                                  message->m_method_name.length() + message->m_err_info.length() +
                                                  message->m_pb_data.length());

    std::string buffer;
    buffer.reserve(pk_len);

    // 1. START
    buffer.push_back(TinyPBProtocol::PB_START);

    // 2. pk_len
    writeInt32(buffer, pk_len);

    // 3. msg_id
    writeInt32(buffer, static_cast<std::int32_t>(message->m_msg_id.length()));
    buffer.append(message->m_msg_id);

    // 4. method_name
    writeInt32(buffer, static_cast<std::int32_t>(message->m_method_name.length()));
    buffer.append(message->m_method_name);

    // 5. err_code
    writeInt32(buffer, message->m_err_code);

    // 6. err_info
    writeInt32(buffer, static_cast<std::int32_t>(message->m_err_info.length()));
    buffer.append(message->m_err_info);

    // 7. pb_data
    buffer.append(message->m_pb_data);

    // 8. checksum
    const std::uint32_t checksum = calculateChecksum(buffer.data(), buffer.size());
    writeInt32(buffer, static_cast<std::int32_t>(checksum));

    // 9. END
    buffer.push_back(TinyPBProtocol::PB_END);

    // 更新协议字段
    message->m_pk_len = pk_len;
    message->m_msg_id_len = static_cast<std::int32_t>(message->m_msg_id.length());
    message->m_method_name_len = static_cast<std::int32_t>(message->m_method_name.length());
    message->m_err_info_len = static_cast<std::int32_t>(message->m_err_info.length());
    message->m_check_sum = static_cast<std::int32_t>(checksum);
    message->parse_success = true;

    return buffer;
}

std::uint32_t TinyPBCoder::calculateChecksum(const char* data, std::size_t len) {
    std::uint32_t crc = CRC::Calculate(data, len, CRC::CRC_32());
    return crc;
}

} // namespace rocket