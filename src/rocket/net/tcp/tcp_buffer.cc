#include "tcp_buffer.h"
#include <algorithm>
#include <array>
#include <bits/types/struct_iovec.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace rocket {

namespace {

inline std::int32_t NetworkToHost32(std::int32_t net32) {
    return static_cast<std::int32_t>(__builtin_bswap32(static_cast<std::uint32_t>(net32)));
}

inline std::int64_t NetworkToHost64(std::int64_t net64) {
    return static_cast<std::int64_t>(__builtin_bswap64(static_cast<std::uint64_t>(net64)));
}

inline std::int32_t HostToNetwork32(std::int32_t host32) {
    return static_cast<std::int32_t>(__builtin_bswap32(static_cast<std::uint32_t>(host32)));
}

inline std::int64_t HostToNetwork64(std::int64_t host64) {
    return static_cast<std::int64_t>(__builtin_bswap64(static_cast<std::uint64_t>(host64)));
}

} // namespace

TcpBuffer::TcpBuffer(std::size_t size) : m_buffer(kPrependSize + size) {}

char* TcpBuffer::begin() noexcept { return m_buffer.data(); }

const char* TcpBuffer::begin() const noexcept { return m_buffer.data(); }

std::size_t TcpBuffer::readAble() const noexcept { return m_write_index - m_read_index; }

std::size_t TcpBuffer::writeAble() const noexcept { return m_buffer.size() - m_write_index; }

std::size_t TcpBuffer::prependAble() const noexcept { return m_read_index; }

std::size_t TcpBuffer::readIndex() const noexcept { return m_read_index; }

std::size_t TcpBuffer::writeIndex() const noexcept { return m_write_index; }

bool TcpBuffer::empty() const noexcept { return m_read_index == m_write_index; }

std::size_t TcpBuffer::capacity() const noexcept { return m_buffer.size(); }

const char* TcpBuffer::peek() const noexcept { return begin() + m_read_index; }

char* TcpBuffer::beginWrite() noexcept { return begin() + m_write_index; }

const char* TcpBuffer::beginWrite() const noexcept { return begin() + m_write_index; }

// ============================================================================
// 写入操作
// ============================================================================

void TcpBuffer::ensureWritable(std::size_t len) {
    if (writeAble() < len) {
        makeSpace(len);
    }
}

void TcpBuffer::makeSpace(std::size_t len) {
    if (writeAble() + prependAble() < len + kPrependSize) {
        const std::size_t new_size = std::min(m_write_index + len, kMaxBufferSize);
        m_buffer.resize(new_size);
    } else {
        const std::size_t readable = readAble();
        std::memmove(begin() + kPrependSize, begin() + m_read_index, readable);
        m_read_index = kPrependSize;
        m_write_index = m_read_index + readable;
    }
}

void TcpBuffer::append(const void* data, std::size_t len) {
    ensureWritable(len);
    std::memcpy(beginWrite(), data, len);
    m_write_index += len;
}

void TcpBuffer::append(std::string_view data) { append(data.data(), data.size()); }

void TcpBuffer::writeToBuffer(const char* buf, std::size_t size) { append(buf, size); }

void TcpBuffer::writeToBuffer(std::string_view data) { append(data); }

void TcpBuffer::appendInt32(std::int32_t x) {
    const std::int32_t net32 = HostToNetwork32(x);
    append(&net32, sizeof(net32));
}

void TcpBuffer::appendInt64(std::int64_t x) {
    const std::int64_t net64 = HostToNetwork64(x);
    append(&net64, sizeof(net64));
}

void TcpBuffer::prepend(const void* data, std::size_t len) {
    if (len > prependAble()) {
        return; // 空间不足，忽略
    }
    m_read_index -= len;
    std::memcpy(begin() + m_read_index, data, len);
}

void TcpBuffer::prependInt32(std::int32_t x) {
    const std::int32_t net32 = HostToNetwork32(x);
    prepend(&net32, sizeof(net32));
}

// ============================================================================
// 读取操作
// ============================================================================

std::span<const char> TcpBuffer::readableSpan() const noexcept { return {peek(), readAble()}; }

std::span<char> TcpBuffer::writableSpan() noexcept { return {beginWrite(), writeAble()}; }

std::string_view TcpBuffer::readableView() const noexcept { return {peek(), readAble()}; }

void TcpBuffer::consume(std::size_t len) {
    if (len < readAble()) {
        m_read_index += len;
    } else {
        consumeAll();
    }
}

void TcpBuffer::consumeAll() noexcept {
    m_read_index = kPrependSize;
    m_write_index = kPrependSize;
}

std::string TcpBuffer::retrieve(std::size_t len) {
    const std::size_t actual = std::min(len, readAble());
    std::string result(peek(), actual);
    consume(actual);
    return result;
}

std::string TcpBuffer::retrieveAll() { return retrieve(readAble()); }

std::string TcpBuffer::retrieveAsString(std::size_t len) { return retrieve(len); }

std::int32_t TcpBuffer::retrieveInt32() {
    const std::int32_t result = peekInt32();
    consume(sizeof(std::int32_t));
    return result;
}

std::int64_t TcpBuffer::retrieveInt64() {
    const std::int64_t result = peekInt64();
    consume(sizeof(std::int64_t));
    return result;
}

std::int32_t TcpBuffer::peekInt32() const {
    std::int32_t net32 = 0;
    std::memcpy(&net32, peek(), sizeof(net32));
    return NetworkToHost32(net32);
}

std::int64_t TcpBuffer::peekInt64() const {
    std::int64_t net64 = 0;
    std::memcpy(&net64, peek(), sizeof(net64));
    return NetworkToHost64(net64);
}

void TcpBuffer::readFromBuffer(std::vector<char>& re, std::size_t size) {
    const std::size_t readable = readAble();
    const std::size_t actual_size = std::min(size, readable);

    re.assign(peek(), peek() + actual_size);
    consume(actual_size);
}

// ============================================================================
// 协议解析辅助
// ============================================================================

std::optional<std::size_t> TcpBuffer::findCRLF() const { return findCRLF(0); }

std::optional<std::size_t> TcpBuffer::findCRLF(std::size_t start) const {
    const char* crlf_pattern = "\r\n";
    const char* crlf = std::search(peek() + start, beginWrite(), crlf_pattern, crlf_pattern + 2);
    if (crlf == beginWrite()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(crlf - peek());
}

std::optional<std::size_t> TcpBuffer::findEOL() const {
    const char* eol = static_cast<const char*>(std::memchr(peek(), '\n', readAble()));
    if (eol == nullptr) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(eol - peek());
}

// ============================================================================
// 缓冲区管理
// ============================================================================

void TcpBuffer::resizeBuffer(std::size_t new_size) {
    std::vector<char> tmp(new_size);
    const std::size_t readable = readAble();

    std::memcpy(tmp.data() + kPrependSize, peek(), readable);

    m_buffer.swap(tmp);
    m_read_index = kPrependSize;
    m_write_index = m_read_index + readable;
}

void TcpBuffer::adjustBuffer() {
    if (prependAble() > kPrependSize && prependAble() > readAble() / 2) {
        const std::size_t readable = readAble();
        std::memmove(begin() + kPrependSize, peek(), readable);
        m_read_index = kPrependSize;
        m_write_index = m_read_index + readable;
    }
}

void TcpBuffer::shrink(std::size_t reserve) {
    std::vector<char> tmp(kPrependSize + readAble() + reserve);
    std::memcpy(tmp.data() + kPrependSize, peek(), readAble());
    m_buffer.swap(tmp);
    m_write_index = kPrependSize + readAble();
    m_read_index = kPrependSize;
}

void TcpBuffer::clear() noexcept { consumeAll(); }

// ============================================================================
// 索引移动（兼容旧接口）
// ============================================================================

void TcpBuffer::moveReadIndex(std::size_t size) { consume(size); }

void TcpBuffer::moveWriteIndex(std::size_t size) {
    const std::size_t actual = std::min(size, writeAble());
    m_write_index += actual;
}

// ============================================================================
// Socket I/O
// ============================================================================

ssize_t TcpBuffer::readFromFd(int fd, int* saved_errno) {
    static constexpr size_t kBufferSize = 65536;
    std::array<char, kBufferSize> extrabuf;
    std::array<struct iovec, 2> vec;

    const std::size_t writable = writeAble();
    vec[0].iov_base = beginWrite();
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf.data();
    vec[1].iov_len = sizeof(extrabuf);

    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec.data(), iovcnt);

    if (n < 0) {
        *saved_errno = errno;
    } else if (static_cast<std::size_t>(n) <= writable) {
        m_write_index += static_cast<std::size_t>(n);
    } else {
        m_write_index = m_buffer.size();
        append(&extrabuf, static_cast<std::size_t>(n) - writable);
    }

    return n;
}

ssize_t TcpBuffer::writeToFd(int fd, int* saved_errno) {
    const ssize_t n = ::write(fd, peek(), readAble());
    if (n < 0) {
        *saved_errno = errno;
    } else {
        consume(static_cast<std::size_t>(n));
    }
    return n;
}

// ============================================================================
// 调试接口
// ============================================================================

const char* TcpBuffer::data() const noexcept { return m_buffer.data(); }

char* TcpBuffer::data() noexcept { return m_buffer.data(); }

} // namespace rocket
