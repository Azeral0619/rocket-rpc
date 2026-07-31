#include "rocket/net/coder/tinypb_coder.h"
#include "3rd-party/CRC.h"
#include "rocket/common/log.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

namespace rocket {

namespace {

constexpr std::size_t kMaxPacketSize = 1024ULL * 1024;

// Raw-pointer variants for stack-buffer encoding (Hical FixedBuffer pattern).
inline char* writeInt32To(char* p, std::int32_t value) {
    std::uint32_t net = htonl(static_cast<std::uint32_t>(value));
    std::memcpy(p, &net, sizeof(net));
    return p + sizeof(net);
}

inline char* appendTo(char* p, const char* data, std::size_t len) {
    if (len > 0) {
        std::memcpy(p, data, len);
        return p + len;
    }
    return p;
}

inline std::int32_t readInt32(const char* data) {
    std::uint32_t net_value = 0;
    std::memcpy(&net_value, data, sizeof(net_value));
    return static_cast<std::int32_t>(ntohl(net_value));
}

} // anonymous namespace

bool TinyPBCoder::encode(std::vector<AbstractProtocol::s_ptr>& messages, TcpBuffer::s_ptr out_buffer) {
    for (auto& msg_base : messages) {
        auto msg = std::dynamic_pointer_cast<TinyPBProtocol>(msg_base);
        if (!msg) {
            ROCKET_LOG_WARN("Skip non-TinyPBProtocol message in encode");
            continue;
        }

        std::string encoded = encodeTinyPB(msg);
        if (!encoded.empty()) {
            if (!out_buffer->writeToBuffer(encoded)) {
                ROCKET_LOG_ERROR("TinyPB output buffer limit exceeded for message [{}]", msg->m_msg_id);
                return false;
            }
            ROCKET_LOG_DEBUG("Encoded TinyPB message [{}], {} bytes", msg->m_msg_id, encoded.length());
        } else {
            return false;
        }
    }
    return true;
}

DecodeResult TinyPBCoder::decode(TcpBuffer::s_ptr buffer) {
    DecodeResult result;

    while (true) {
        const std::size_t readable = buffer->readAble();
        if (readable == 0) {
            return result;
        }

        std::vector<char> tmp;
        tmp.resize(readable);
        std::memcpy(tmp.data(), buffer->readableSpan().data(), readable);

        ROCKET_LOG_DEBUG("Decode: readable={} bytes, first byte=0x{:02x}", readable,
                         static_cast<unsigned char>(tmp[0]));

        const auto start_it = std::find(tmp.begin(), tmp.end(), TinyPBProtocol::PB_START);
        if (start_it == tmp.end()) {
            // No possible frame remains; do not retain an unbounded garbage
            // prefix while waiting for the next socket read.
            buffer->moveReadIndex(readable);
            return result;
        }

        const std::size_t pk_start_index =
            static_cast<std::size_t>(std::distance(tmp.begin(), start_it));
        const std::size_t bytes_from_start = readable - pk_start_index;
        if (bytes_from_start < 1 + sizeof(std::int32_t)) {
            buffer->moveReadIndex(pk_start_index);
            return result;
        }

        const std::int32_t pk_len = readInt32(&tmp[pk_start_index + 1]);
        ROCKET_LOG_DEBUG("Found PB_START at {}, pk_len={}", pk_start_index, pk_len);

        if (pk_len < static_cast<std::int32_t>(TinyPBProtocol::HEADER_SIZE) ||
            static_cast<std::size_t>(pk_len) > kMaxPacketSize) {
            ROCKET_LOG_WARN("Invalid pk_len={}, resynchronizing", pk_len);
            buffer->moveReadIndex(pk_start_index + 1);
            continue;
        }

        const std::size_t packet_size = static_cast<std::size_t>(pk_len);
        if (bytes_from_start < packet_size) {
            // Consume only the confirmed garbage prefix and retain the
            // incomplete frame.
            buffer->moveReadIndex(pk_start_index);
            return result;
        }

        const std::size_t pk_end_index = pk_start_index + packet_size - 1;
        if (tmp[pk_end_index] != TinyPBProtocol::PB_END) {
            ROCKET_LOG_WARN("PB_END not found at expected position {}, resynchronizing", pk_end_index);
            buffer->moveReadIndex(pk_start_index + 1);
            continue;
        }

        auto message = std::make_shared<TinyPBProtocol>();
        message->m_pk_len = pk_len;

        std::size_t parse_index = pk_start_index + 1 + sizeof(std::int32_t);

        auto checkRemaining = [&](std::size_t need_bytes) -> bool {
            if (parse_index > pk_end_index || need_bytes > pk_end_index - parse_index) {
                ROCKET_LOG_ERROR("Parse error at index {}, need {} bytes", parse_index, need_bytes);
                return false;
            }
            return true;
        };

        auto rejectPacket = [&](std::string_view reason) {
            ROCKET_LOG_ERROR("Reject TinyPB packet: {}", reason);
            message->parse_success = false;
            buffer->moveReadIndex(pk_end_index + 1);
            result.fatal = true;
        };

        auto readLength = [&](std::int32_t& value, std::string_view field_name) -> bool {
            if (!checkRemaining(sizeof(std::int32_t))) {
                rejectPacket("missing " + std::string(field_name) + " length");
                return false;
            }
            value = readInt32(&tmp[parse_index]);
            parse_index += sizeof(std::int32_t);
            if (value < 0) {
                rejectPacket("negative " + std::string(field_name) + " length");
                return false;
            }
            if (!checkRemaining(static_cast<std::size_t>(value))) {
                rejectPacket("oversized " + std::string(field_name) + " length");
                return false;
            }
            return true;
        };

        // 1. msg_id
        if (!readLength(message->m_msg_id_len, "msg_id")) {
            return result;
        }
        message->m_msg_id.assign(&tmp[parse_index], static_cast<std::size_t>(message->m_msg_id_len));
        parse_index += static_cast<std::size_t>(message->m_msg_id_len);
        ROCKET_LOG_DEBUG("msg_id: {}", message->m_msg_id);

        // 2. method_name
        if (!readLength(message->m_method_name_len, "method_name")) {
            return result;
        }
        message->m_method_name.assign(&tmp[parse_index],
                                      static_cast<std::size_t>(message->m_method_name_len));
        parse_index += static_cast<std::size_t>(message->m_method_name_len);
        ROCKET_LOG_DEBUG("method: {}", message->m_method_name);

        // 3. err_code
        if (!checkRemaining(sizeof(std::int32_t))) {
            rejectPacket("missing error code");
            return result;
        }
        message->m_err_code = readInt32(&tmp[parse_index]);
        parse_index += sizeof(std::int32_t);

        // 4. err_info
        if (!readLength(message->m_err_info_len, "err_info")) {
            return result;
        }
        message->m_err_info.assign(&tmp[parse_index],
                                   static_cast<std::size_t>(message->m_err_info_len));
        parse_index += static_cast<std::size_t>(message->m_err_info_len);

        // 5. pb_data
        if (parse_index > pk_end_index ||
            sizeof(std::int32_t) > pk_end_index - parse_index) {
            rejectPacket("missing checksum");
            return result;
        }
        const std::size_t checksum_index = pk_end_index - sizeof(std::int32_t);
        if (parse_index > checksum_index) {
            rejectPacket("invalid protobuf payload length");
            return result;
        }
        message->m_pb_data.assign(&tmp[parse_index], checksum_index - parse_index);
        parse_index = checksum_index;

        // 6. checksum
        if (!checkRemaining(sizeof(std::int32_t))) {
            rejectPacket("missing checksum");
            return result;
        }
        message->m_check_sum = readInt32(&tmp[parse_index]);
        const auto calculated =
            calculateChecksum(&tmp[pk_start_index], checksum_index - pk_start_index);
        if (calculated != static_cast<std::uint32_t>(message->m_check_sum)) {
            rejectPacket("checksum mismatch");
            return result;
        }

        // Consume both the valid frame and any garbage prefix preceding it.
        buffer->moveReadIndex(pk_end_index + 1);
        message->parse_success = true;
        result.messages.push_back(message);

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

    const std::size_t packet_size = TinyPBProtocol::HEADER_SIZE + message->m_msg_id.length() +
                                    message->m_method_name.length() + message->m_err_info.length() +
                                    message->m_pb_data.length();
    if (packet_size > kMaxPacketSize ||
        packet_size > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        ROCKET_LOG_ERROR("TinyPB packet too large: {} bytes", packet_size);
        return "";
    }
    const auto pk_len = static_cast<std::int32_t>(packet_size);

    // Stack buffer for the common case (most RPC messages are < 512 bytes).
    // Avoids heap allocation on the encode hot path.  Pattern from Hical's
    // FixedBuffer used for HTTP response serialization.
    constexpr std::size_t kStackCapacity = 512;
    char stack_buf[kStackCapacity];
    char* buf;
    std::string heap_buf;

    if (static_cast<std::size_t>(pk_len) <= kStackCapacity) {
        buf = stack_buf;
    } else {
        heap_buf.resize(static_cast<std::size_t>(pk_len));
        buf = heap_buf.data();
    }

    char* p = buf;

    // 1. START
    *p++ = TinyPBProtocol::PB_START;

    // 2. pk_len
    p = writeInt32To(p, pk_len);

    // 3. msg_id
    p = writeInt32To(p, static_cast<std::int32_t>(message->m_msg_id.length()));
    p = appendTo(p, message->m_msg_id.data(), message->m_msg_id.length());

    // 4. method_name
    p = writeInt32To(p, static_cast<std::int32_t>(message->m_method_name.length()));
    p = appendTo(p, message->m_method_name.data(), message->m_method_name.length());

    // 5. err_code
    p = writeInt32To(p, message->m_err_code);

    // 6. err_info
    p = writeInt32To(p, static_cast<std::int32_t>(message->m_err_info.length()));
    p = appendTo(p, message->m_err_info.data(), message->m_err_info.length());

    // 7. pb_data
    p = appendTo(p, message->m_pb_data.data(), message->m_pb_data.length());

    // 8. checksum
    const std::size_t body_len = static_cast<std::size_t>(p - buf);
    const std::uint32_t checksum = calculateChecksum(buf, body_len);
    p = writeInt32To(p, static_cast<std::int32_t>(checksum));

    // 9. END
    *p++ = TinyPBProtocol::PB_END;

    // 更新协议字段
    message->m_pk_len = pk_len;
    message->m_msg_id_len = static_cast<std::int32_t>(message->m_msg_id.length());
    message->m_method_name_len = static_cast<std::int32_t>(message->m_method_name.length());
    message->m_err_info_len = static_cast<std::int32_t>(message->m_err_info.length());
    message->m_check_sum = static_cast<std::int32_t>(checksum);
    message->parse_success = true;

    const std::size_t total_len = static_cast<std::size_t>(p - buf);
    if (static_cast<std::size_t>(pk_len) <= kStackCapacity) {
        return std::string(stack_buf, total_len);
    }
    heap_buf.resize(total_len);
    return heap_buf;
}

std::uint32_t TinyPBCoder::calculateChecksum(const char* data, std::size_t len) {
    std::uint32_t crc = CRC::Calculate(data, len, CRC::CRC_32());
    return crc;
}

} // namespace rocket
