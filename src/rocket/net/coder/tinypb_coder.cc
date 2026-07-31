#include "rocket/net/coder/tinypb_coder.h"
#include "3rd-party/CRC.h"
#include "rocket/common/log.h"
#include "rocket/common/msg_id_util.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <google/protobuf/message.h>
#include <span>
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

inline char* writeMessageIdTo(char* p, MessageId value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    std::memcpy(p, &value, sizeof(value));
    return p + sizeof(value);
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

inline MessageId readMessageId(const char* data) {
    MessageId value = 0;
    std::memcpy(&value, data, sizeof(value));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return value;
}

} // anonymous namespace

bool TinyPBCoder::encode(
    std::span<const AbstractProtocol::s_ptr> messages,
    TcpBuffer& out_buffer) {
    for (const auto& msg_base : messages) {
        auto* msg = dynamic_cast<TinyPBProtocol*>(msg_base.get());
        if (msg == nullptr) {
            ROCKET_LOG_WARN("Skip non-TinyPBProtocol message in encode");
            continue;
        }

        if (msg->m_msg_id == kInvalidMessageId) {
            msg->m_msg_id = MsgIDUtil::GenMsgID();
        }

        const auto* protobuf_message = msg->protobufMessage();
        const std::string_view protobuf_bytes = msg->pbDataView();
        const std::size_t protobuf_size =
            protobuf_message != nullptr ? protobuf_message->ByteSizeLong()
                                        : protobuf_bytes.size();
        const std::size_t fields_size =
            TinyPBProtocol::HEADER_SIZE + msg->m_method_name.length() +
            msg->m_err_info.length();
        if (fields_size > kMaxPacketSize ||
            protobuf_size > kMaxPacketSize - fields_size) {
            ROCKET_LOG_ERROR("TinyPB packet too large: {} bytes",
                             fields_size + protobuf_size);
            return false;
        }
        const std::size_t packet_size = fields_size + protobuf_size;
        if (packet_size > kMaxPacketSize ||
            packet_size >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            ROCKET_LOG_ERROR("TinyPB packet too large: {} bytes", packet_size);
            return false;
        }
        if (!out_buffer.ensureWritable(packet_size)) {
            ROCKET_LOG_ERROR("TinyPB output buffer limit exceeded for message [{}]",
                             msg->m_msg_id);
            return false;
        }

        const auto pk_len = static_cast<std::int32_t>(packet_size);
        char* const buf = out_buffer.beginWrite();
        char* p = buf;

        *p++ = TinyPBProtocol::PB_START;
        p = writeInt32To(p, pk_len);
        p = writeMessageIdTo(p, msg->m_msg_id);
        p = writeInt32To(
            p, static_cast<std::int32_t>(msg->m_method_name.length()));
        p = appendTo(p, msg->m_method_name.data(), msg->m_method_name.length());
        p = writeInt32To(p, msg->m_err_code);
        p = writeInt32To(p, static_cast<std::int32_t>(msg->m_err_info.length()));
        p = appendTo(p, msg->m_err_info.data(), msg->m_err_info.length());
        if (protobuf_message != nullptr) {
            // ByteSizeLong() above populated protobuf's cached sizes. The RPC
            // caller already checked IsInitialized(), so avoid SerializeToArray
            // repeating both operations before writing the same bytes.
            auto* const payload_begin =
                reinterpret_cast<std::uint8_t*>(p);
            auto* const payload_end =
                protobuf_message->SerializeWithCachedSizesToArray(
                    payload_begin);
            if (payload_end != payload_begin + protobuf_size) {
                ROCKET_LOG_ERROR(
                    "TinyPB protobuf serialization failed for message [{}]",
                    msg->m_msg_id);
                return false;
            }
            p = reinterpret_cast<char*>(payload_end);
        } else {
            p = appendTo(p, protobuf_bytes.data(), protobuf_bytes.size());
        }

        const std::size_t body_len = static_cast<std::size_t>(p - buf);
        const std::uint32_t checksum =
            m_checksum_policy == ChecksumPolicy::Crc32
                ? calculateChecksum(buf, body_len)
                : 0;
        p = writeInt32To(p, static_cast<std::int32_t>(checksum));
        *p++ = TinyPBProtocol::PB_END;

        const std::size_t total_len = static_cast<std::size_t>(p - buf);
        out_buffer.moveWriteIndex(total_len);

        msg->m_pk_len = pk_len;
        msg->m_method_name_len =
            static_cast<std::int32_t>(msg->m_method_name.length());
        msg->m_err_info_len =
            static_cast<std::int32_t>(msg->m_err_info.length());
        msg->m_check_sum = static_cast<std::int32_t>(checksum);
        msg->parse_success = true;

        ROCKET_LOG_DEBUG("Encoded TinyPB message [{}], {} bytes",
                         msg->m_msg_id, total_len);
    }
    return true;
}

bool TinyPBCoder::decode(
    TcpBuffer& buffer, std::vector<AbstractProtocol::s_ptr>& output) {
    while (true) {
        const std::size_t readable = buffer.readAble();
        if (readable == 0) {
            return true;
        }

        // TcpBuffer keeps its readable bytes contiguous.  Parse that storage
        // directly instead of allocating a temporary vector and copying the
        // entire receive batch on every decode pass.
        const auto readable_bytes = buffer.readableSpan();
        const char* const data = readable_bytes.data();

        ROCKET_LOG_DEBUG("Decode: readable={} bytes, first byte=0x{:02x}", readable,
                         static_cast<unsigned char>(data[0]));

        const char* const end = data + readable;
        const auto start_it = std::find(data, end, TinyPBProtocol::PB_START);
        if (start_it == end) {
            // No possible frame remains; do not retain an unbounded garbage
            // prefix while waiting for the next socket read.
            buffer.moveReadIndex(readable);
            return true;
        }

        const std::size_t pk_start_index =
            static_cast<std::size_t>(start_it - data);
        const std::size_t bytes_from_start = readable - pk_start_index;
        if (bytes_from_start < 1 + sizeof(std::int32_t)) {
            buffer.moveReadIndex(pk_start_index);
            return true;
        }

        const std::int32_t pk_len = readInt32(data + pk_start_index + 1);
        ROCKET_LOG_DEBUG("Found PB_START at {}, pk_len={}", pk_start_index, pk_len);

        if (pk_len < static_cast<std::int32_t>(TinyPBProtocol::HEADER_SIZE) ||
            static_cast<std::size_t>(pk_len) > kMaxPacketSize) {
            ROCKET_LOG_WARN("Invalid pk_len={}, resynchronizing", pk_len);
            buffer.moveReadIndex(pk_start_index + 1);
            continue;
        }

        const std::size_t packet_size = static_cast<std::size_t>(pk_len);
        if (bytes_from_start < packet_size) {
            // Consume only the confirmed garbage prefix and retain the
            // incomplete frame.
            buffer.moveReadIndex(pk_start_index);
            return true;
        }

        const std::size_t pk_end_index = pk_start_index + packet_size - 1;
        if (data[pk_end_index] != TinyPBProtocol::PB_END) {
            ROCKET_LOG_WARN("PB_END not found at expected position {}, resynchronizing", pk_end_index);
            buffer.moveReadIndex(pk_start_index + 1);
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
            buffer.moveReadIndex(pk_end_index + 1);
        };

        auto readLength = [&](std::int32_t& value, std::string_view field_name) -> bool {
            if (!checkRemaining(sizeof(std::int32_t))) {
                rejectPacket("missing " + std::string(field_name) + " length");
                return false;
            }
            value = readInt32(data + parse_index);
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

        // 1. fixed-width message ID
        if (!checkRemaining(sizeof(MessageId))) {
            rejectPacket("missing message id");
            return false;
        }
        message->m_msg_id = readMessageId(data + parse_index);
        parse_index += sizeof(MessageId);
        if (message->m_msg_id == kInvalidMessageId) {
            rejectPacket("invalid message id");
            return false;
        }
        ROCKET_LOG_DEBUG("msg_id: {}", message->m_msg_id);

        // 2. method_name
        if (!readLength(message->m_method_name_len, "method_name")) {
            return false;
        }
        message->m_method_name.assign(data + parse_index,
                                      static_cast<std::size_t>(message->m_method_name_len));
        parse_index += static_cast<std::size_t>(message->m_method_name_len);
        ROCKET_LOG_DEBUG("method: {}", message->m_method_name);

        // 3. err_code
        if (!checkRemaining(sizeof(std::int32_t))) {
            rejectPacket("missing error code");
            return false;
        }
        message->m_err_code = readInt32(data + parse_index);
        parse_index += sizeof(std::int32_t);

        // 4. err_info
        if (!readLength(message->m_err_info_len, "err_info")) {
            return false;
        }
        message->m_err_info.assign(data + parse_index,
                                   static_cast<std::size_t>(message->m_err_info_len));
        parse_index += static_cast<std::size_t>(message->m_err_info_len);

        // 5. pb_data
        if (parse_index > pk_end_index ||
            sizeof(std::int32_t) > pk_end_index - parse_index) {
            rejectPacket("missing checksum");
            return false;
        }
        const std::size_t checksum_index = pk_end_index - sizeof(std::int32_t);
        if (parse_index > checksum_index) {
            rejectPacket("invalid protobuf payload length");
            return false;
        }
        const std::string_view protobuf_payload(
            data + parse_index, checksum_index - parse_index);
        if (m_payload_mode == PayloadMode::Borrowed) {
            message->setBorrowedPbData(protobuf_payload);
        } else {
            message->m_pb_data.assign(protobuf_payload);
        }
        parse_index = checksum_index;

        // 6. checksum
        if (!checkRemaining(sizeof(std::int32_t))) {
            rejectPacket("missing checksum");
            return false;
        }
        message->m_check_sum = readInt32(data + parse_index);
        if (m_checksum_policy == ChecksumPolicy::Crc32) {
            const auto calculated = calculateChecksum(
                data + pk_start_index, checksum_index - pk_start_index);
            if (calculated !=
                static_cast<std::uint32_t>(message->m_check_sum)) {
                rejectPacket("checksum mismatch");
                return false;
            }
        }

        // Consume both the valid frame and any garbage prefix preceding it.
        buffer.moveReadIndex(pk_end_index + 1);
        message->parse_success = true;
        ROCKET_LOG_DEBUG("Decoded message [{}], method: {}", message->m_msg_id,
                         message->m_method_name);
        output.push_back(std::move(message));
    }
}

std::uint32_t TinyPBCoder::calculateChecksum(const char* data, std::size_t len) {
    // Passing CRC_32() directly selects CRC++'s bit-at-a-time overload,
    // which performs eight polynomial steps for every byte in every frame.
    // Build the 256-entry table once and use the byte-at-a-time overload;
    // both paths implement the exact same CRC-32 parameters and wire value.
    static const CRC::Table<std::uint32_t, 32> table(CRC::CRC_32());
    return CRC::Calculate(data, len, table);
}

} // namespace rocket
