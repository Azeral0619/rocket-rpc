#include "rocket/net/tcp/tcp_buffer.h"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace rocket {
namespace {

TEST(TcpBuffer, AppendRetrieveRoundtrip) {
    TcpBuffer buf;
    buf.append("hello", 5);
    EXPECT_EQ(buf.readAble(), 5U);
    EXPECT_EQ(buf.readableView(), "hello");

    const std::string out = buf.retrieve(3);
    EXPECT_EQ(out, "hel");
    EXPECT_EQ(buf.readAble(), 2U);
    EXPECT_EQ(buf.retrieveAll(), "lo");
    EXPECT_TRUE(buf.empty());
}

TEST(TcpBuffer, Int32NetworkByteOrder) {
    TcpBuffer buf;
    buf.appendInt32(0x01020304);
    ASSERT_EQ(buf.readAble(), 4U);
    // Network byte order: big-endian on the wire.
    const auto span = buf.readableSpan();
    EXPECT_EQ(static_cast<unsigned char>(span[0]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(span[3]), 0x04);
    EXPECT_EQ(buf.peekInt32(), 0x01020304);
    EXPECT_EQ(buf.retrieveInt32(), 0x01020304);
    EXPECT_TRUE(buf.empty());
}

TEST(TcpBuffer, Int64Roundtrip) {
    TcpBuffer buf;
    buf.appendInt64(0x0102030405060708LL);
    EXPECT_EQ(buf.peekInt64(), 0x0102030405060708LL);
    EXPECT_EQ(buf.retrieveInt64(), 0x0102030405060708LL);
}

TEST(TcpBuffer, Prepend) {
    TcpBuffer buf;
    buf.append("world", 5);
    buf.prependInt32(5);
    EXPECT_EQ(buf.readAble(), 9U);
    EXPECT_EQ(buf.retrieveInt32(), 5);
    EXPECT_EQ(buf.retrieveAll(), "world");
}

TEST(TcpBuffer, MakeSpaceMovesReadIndexBack) {
    TcpBuffer buf(16);
    buf.append("0123456789abcdef", 16); // fills initial capacity
    EXPECT_EQ(buf.retrieve(12), "0123456789ab");
    // Now writable space is short; append should move data back to prepend area.
    buf.append("XXXX", 4);
    EXPECT_EQ(buf.readAble(), 8U);
    EXPECT_EQ(buf.retrieveAll(), "cdefXXXX");
}

TEST(TcpBuffer, GrowBeyondInitialCapacity) {
    TcpBuffer buf(16);
    const std::string big(4096, 'x');
    buf.append(big);
    EXPECT_EQ(buf.readAble(), 4096U);
    EXPECT_EQ(buf.retrieveAll(), big);
}

TEST(TcpBuffer, FindEOLAndCRLF) {
    TcpBuffer buf;
    buf.append("line1\r\nline2\n", 13);
    EXPECT_EQ(buf.findCRLF(), 5U);
    EXPECT_EQ(buf.findEOL(), 6U); // first '\n' is at index 6 (right after \r)
    EXPECT_EQ(buf.findCRLF(6), std::nullopt);
}

TEST(TcpBuffer, ConsumeClampsToReadable) {
    TcpBuffer buf;
    buf.append("ab", 2);
    buf.consume(100); // more than readable -> consumeAll
    EXPECT_TRUE(buf.empty());
}

TEST(TcpBuffer, MoveWriteIndexClamps) {
    TcpBuffer buf(8);
    buf.moveWriteIndex(1000); // must not overrun
    EXPECT_LE(buf.writeIndex(), buf.capacity());
}

TEST(TcpBuffer, ReadFromFdUsesExtraBuffer) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    // The socketpair buffer is far smaller than the payload, so a single
    // blocking write would deadlock — run both ends non-blocking and pump.
    for (const int fd : fds) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ASSERT_NE(flags, -1);
        ASSERT_NE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), -1);
    }

    TcpBuffer buf(16); // writable space much smaller than the 64KB extrabuf
    const std::string payload(100000, 'p');

    std::size_t written = 0;
    std::size_t total_read = 0;
    int saved_errno = 0;
    while (total_read < payload.size()) {
        if (written < payload.size()) {
            const ssize_t n = ::write(fds[0], payload.data() + written, payload.size() - written);
            if (n > 0) {
                written += static_cast<std::size_t>(n);
            }
        }
        const ssize_t r = buf.readFromFd(fds[1], &saved_errno);
        if (r > 0) {
            total_read += static_cast<std::size_t>(r);
        } else if (r == 0) {
            FAIL() << "unexpected EOF";
            break;
        } else {
            ASSERT_TRUE(saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) << "errno=" << saved_errno;
        }
    }
    EXPECT_EQ(buf.readAble(), payload.size());
    EXPECT_EQ(buf.retrieveAll(), payload);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(TcpBuffer, WriteToFdConsumesWrittenBytes) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    TcpBuffer buf;
    buf.append("ping", 4);
    int saved_errno = 0;
    EXPECT_EQ(buf.writeToFd(fds[0], &saved_errno), 4);
    EXPECT_TRUE(buf.empty());

    char out[4] = {};
    EXPECT_EQ(::read(fds[1], out, 4), 4);
    EXPECT_EQ(std::string(out, 4), "ping");

    ::close(fds[0]);
    ::close(fds[1]);
}

} // namespace
} // namespace rocket
