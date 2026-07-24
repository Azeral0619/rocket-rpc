#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace rocket {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;

TinyPBProtocol::s_ptr MakeMessage(std::string_view msg_id, std::string_view method, std::string_view pb_data,
                                  std::int32_t err_code = 0, std::string_view err_info = "") {
    auto msg = std::make_shared<TinyPBProtocol>();
    msg->m_msg_id = std::string(msg_id);
    msg->m_method_name = std::string(method);
    msg->m_pb_data = std::string(pb_data);
    msg->m_err_code = err_code;
    msg->m_err_info = std::string(err_info);
    return msg;
}

TEST(TinyPBCoder, Roundtrip) {
    TinyPBCoder coder;
    TcpBuffer buf;

    auto original = MakeMessage("12345", "Order.makeOrder", "payload");
    std::vector<AbstractProtocol::s_ptr> messages = {original};
    EXPECT_TRUE(coder.encode(messages, buf));

    auto result = coder.decode(buf);
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(result.dropped_frames, 0U);
    ASSERT_THAT(result.messages, SizeIs(1));

    auto decoded = std::dynamic_pointer_cast<TinyPBProtocol>(result.messages[0]);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->m_msg_id, "12345");
    EXPECT_EQ(decoded->m_method_name, "Order.makeOrder");
    EXPECT_EQ(decoded->m_pb_data, "payload");
    EXPECT_EQ(decoded->m_err_code, 0);
    EXPECT_TRUE(buf.empty());
}

TEST(TinyPBCoder, MultipleFramesInOneBuffer) {
    TinyPBCoder coder;
    TcpBuffer buf;

    std::vector<AbstractProtocol::s_ptr> messages = {
        MakeMessage("1", "A.m1", "one"),
        MakeMessage("2", "A.m2", "two"),
        MakeMessage("3", "A.m3", "three"),
    };
    EXPECT_TRUE(coder.encode(messages, buf));

    auto result = coder.decode(buf);
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(result.dropped_frames, 0U);
    ASSERT_THAT(result.messages, SizeIs(3));
    EXPECT_EQ(std::dynamic_pointer_cast<TinyPBProtocol>(result.messages[0])->m_pb_data, "one");
    EXPECT_EQ(std::dynamic_pointer_cast<TinyPBProtocol>(result.messages[1])->m_pb_data, "two");
    EXPECT_EQ(std::dynamic_pointer_cast<TinyPBProtocol>(result.messages[2])->m_pb_data, "three");
    EXPECT_TRUE(buf.empty());
}

TEST(TinyPBCoder, IncompleteFrameWaitsForMoreData) {
    TinyPBCoder coder;
    TcpBuffer buf;

    auto original = MakeMessage("id", "Method", "payload");
    std::vector<AbstractProtocol::s_ptr> messages = {original};
    EXPECT_TRUE(coder.encode(messages, buf));

    // Take a prefix of the encoded frame.
    const std::string encoded = buf.retrieveAll();
    ASSERT_FALSE(encoded.empty());
    buf.append(encoded.data(), encoded.size() / 2);

    auto result = coder.decode(buf);
    EXPECT_FALSE(result.fatal);
    EXPECT_THAT(result.messages, IsEmpty());
    EXPECT_FALSE(buf.empty());

    // Append the rest and decode.
    buf.append(encoded.data() + encoded.size() / 2, encoded.size() - encoded.size() / 2);
    result = coder.decode(buf);
    EXPECT_FALSE(result.fatal);
    ASSERT_THAT(result.messages, SizeIs(1));
    EXPECT_EQ(std::dynamic_pointer_cast<TinyPBProtocol>(result.messages[0])->m_msg_id, "id");
    EXPECT_TRUE(buf.empty());
}

TEST(TinyPBCoder, CrcMismatchDropsFrame) {
    TinyPBCoder coder;
    TcpBuffer buf;

    auto original = MakeMessage("id", "Method", "payload");
    std::vector<AbstractProtocol::s_ptr> messages = {original};
    EXPECT_TRUE(coder.encode(messages, buf));

    std::string encoded = buf.retrieveAll();
    // Tamper with the payload byte before the checksum.
    ASSERT_GT(encoded.size(), 10U);
    encoded[encoded.size() - 5] ^= 0xFF;
    buf.append(encoded);

    auto result = coder.decode(buf);
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(result.dropped_frames, 1U);
    EXPECT_THAT(result.messages, IsEmpty());
}

TEST(TinyPBCoder, ThreeConsecutiveErrorsBecomeFatal) {
    TinyPBCoder coder;

    for (int i = 0; i < 3; ++i) {
        TcpBuffer buf;
        auto original = MakeMessage(std::to_string(i), "Method", "payload");
        std::vector<AbstractProtocol::s_ptr> messages = {original};
        EXPECT_TRUE(coder.encode(messages, buf));

        std::string encoded = buf.retrieveAll();
        ASSERT_GT(encoded.size(), 10U);
        encoded[encoded.size() - 5] ^= 0xFF;
        buf.append(encoded);

        auto result = coder.decode(buf);
        if (i < 2) {
            EXPECT_FALSE(result.fatal) << "iteration " << i;
        } else {
            EXPECT_TRUE(result.fatal) << "iteration " << i;
        }
    }
}

TEST(TinyPBCoder, ErrorsResetAfterSuccessfulFrame) {
    TinyPBCoder coder;

    // Two bad frames.
    for (int i = 0; i < 2; ++i) {
        TcpBuffer buf;
        auto original = MakeMessage(std::to_string(i), "Method", "payload");
        std::vector<AbstractProtocol::s_ptr> messages = {original};
        EXPECT_TRUE(coder.encode(messages, buf));

        std::string encoded = buf.retrieveAll();
        encoded[encoded.size() - 5] ^= 0xFF;
        buf.append(encoded);

        auto result = coder.decode(buf);
        EXPECT_FALSE(result.fatal);
    }

    // One good frame resets the counter.
    {
        TcpBuffer buf;
        auto original = MakeMessage("good", "Method", "payload");
        std::vector<AbstractProtocol::s_ptr> messages = {original};
        EXPECT_TRUE(coder.encode(messages, buf));

        auto result = coder.decode(buf);
        EXPECT_FALSE(result.fatal);
        EXPECT_EQ(result.dropped_frames, 0U);
        ASSERT_THAT(result.messages, SizeIs(1));
    }

    // Three more bad frames are needed to trigger fatal again.
    for (int i = 0; i < 3; ++i) {
        TcpBuffer buf;
        auto original = MakeMessage(std::to_string(i), "Method", "payload");
        std::vector<AbstractProtocol::s_ptr> messages = {original};
        EXPECT_TRUE(coder.encode(messages, buf));

        std::string encoded = buf.retrieveAll();
        encoded[encoded.size() - 5] ^= 0xFF;
        buf.append(encoded);

        auto result = coder.decode(buf);
        if (i < 2) {
            EXPECT_FALSE(result.fatal);
        } else {
            EXPECT_TRUE(result.fatal);
        }
    }
}

TEST(TinyPBCoder, GiantPkLenDoesNotCrash) {
    TinyPBCoder coder;
    TcpBuffer buf;

    // Craft: START + pk_len = 1GB + END placeholder.
    char garbage[10] = {};
    garbage[0] = TinyPBProtocol::PB_START;
    std::uint32_t giant = htonl(1024 * 1024 * 1024);
    std::memcpy(garbage + 1, &giant, sizeof(giant));
    garbage[5] = TinyPBProtocol::PB_END;
    buf.append(garbage, sizeof(garbage));

    auto result = coder.decode(buf);
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(result.dropped_frames, 1U);
    EXPECT_THAT(result.messages, IsEmpty());
    // The invalid START byte is consumed as one error; the trailing bytes are
    // garbage with no START marker and are also discarded during resync.
    EXPECT_TRUE(buf.empty());
}

TEST(TinyPBCoder, ResyncsAfterGarbagePrefix) {
    TinyPBCoder coder;
    TcpBuffer buf;

    auto original = MakeMessage("id", "Method", "payload");
    std::vector<AbstractProtocol::s_ptr> messages = {original};
    EXPECT_TRUE(coder.encode(messages, buf));

    std::string encoded = buf.retrieveAll();
    std::string prefixed = std::string("garbage") + encoded;
    buf.append(prefixed);

    auto result = coder.decode(buf);
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(result.dropped_frames, 0U);
    ASSERT_THAT(result.messages, SizeIs(1));
    EXPECT_TRUE(buf.empty());
}

TEST(TinyPBCoder, FormatLock) {
    TinyPBCoder coder;
    TcpBuffer buf;

    auto original = MakeMessage("abc", "X.y", "data");
    std::vector<AbstractProtocol::s_ptr> messages = {original};
    EXPECT_TRUE(coder.encode(messages, buf));

    const std::string encoded = buf.retrieveAll();
    ASSERT_GE(encoded.size(), 27U);

    // START
    EXPECT_EQ(static_cast<unsigned char>(encoded[0]), 0x02U);
    // END
    EXPECT_EQ(static_cast<unsigned char>(encoded[encoded.size() - 1]), 0x03U);

    // pk_len is encoded right after START.
    std::uint32_t net_pk_len = 0;
    std::memcpy(&net_pk_len, encoded.data() + 1, sizeof(net_pk_len));
    const std::int32_t pk_len = static_cast<std::int32_t>(ntohl(net_pk_len));
    EXPECT_EQ(pk_len, static_cast<std::int32_t>(encoded.size()));
}

} // namespace
} // namespace rocket
