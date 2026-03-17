#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace rocket {

class TcpBuffer {

  public:
    using s_ptr = std::shared_ptr<TcpBuffer>;

    static constexpr std::size_t kDefaultSize = 1024;
    static constexpr std::size_t kPrependSize = 8;
    static constexpr std::size_t kMaxBufferSize = 64ULL * 1024 * 1024;

    TcpBuffer() : TcpBuffer(kDefaultSize) {}

    explicit TcpBuffer(std::size_t size);

    ~TcpBuffer() = default;

    TcpBuffer(const TcpBuffer&) = delete;
    TcpBuffer& operator=(const TcpBuffer&) = delete;
    TcpBuffer(TcpBuffer&&) noexcept = default;
    TcpBuffer& operator=(TcpBuffer&&) noexcept = default;

    [[nodiscard]] std::size_t readAble() const noexcept;
    [[nodiscard]] std::size_t writeAble() const noexcept;
    [[nodiscard]] std::size_t prependAble() const noexcept;
    [[nodiscard]] std::size_t readIndex() const noexcept;
    [[nodiscard]] std::size_t writeIndex() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

    void writeToBuffer(const char* buf, std::size_t size);
    void writeToBuffer(std::string_view data);
    void append(const void* data, std::size_t len);
    void append(std::string_view data);
    void appendInt32(std::int32_t x);
    void appendInt64(std::int64_t x);

    void prepend(const void* data, std::size_t len);
    void prependInt32(std::int32_t x);

    [[nodiscard]] std::span<const char> readableSpan() const noexcept;
    [[nodiscard]] std::span<char> writableSpan() noexcept;
    [[nodiscard]] std::string_view readableView() const noexcept;
    [[nodiscard]] const char* peek() const noexcept;
    [[nodiscard]] char* beginWrite() noexcept;
    [[nodiscard]] const char* beginWrite() const noexcept;

    void readFromBuffer(std::vector<char>& re, std::size_t size);
    [[nodiscard]] std::string retrieve(std::size_t len);
    [[nodiscard]] std::string retrieveAll();
    [[nodiscard]] std::string retrieveAsString(std::size_t len);
    [[nodiscard]] std::int32_t retrieveInt32();
    [[nodiscard]] std::int64_t retrieveInt64();

    [[nodiscard]] std::int32_t peekInt32() const;
    [[nodiscard]] std::int64_t peekInt64() const;

    void consume(std::size_t len);
    void consumeAll() noexcept;

    [[nodiscard]] std::optional<std::size_t> findCRLF() const;
    [[nodiscard]] std::optional<std::size_t> findCRLF(std::size_t start) const;
    [[nodiscard]] std::optional<std::size_t> findEOL() const;

    void resizeBuffer(std::size_t new_size);
    void adjustBuffer();
    void ensureWritable(std::size_t len);
    void shrink(std::size_t reserve);
    void clear() noexcept;

    // 索引移动（兼容旧接口）
    void moveReadIndex(std::size_t size);
    void moveWriteIndex(std::size_t size);

    [[nodiscard]] ssize_t readFromFd(int fd, int* saved_errno);
    [[nodiscard]] ssize_t writeToFd(int fd, int* saved_errno);

    [[nodiscard]] const char* data() const noexcept;
    [[nodiscard]] char* data() noexcept;

  private:
    void makeSpace(std::size_t len);
    [[nodiscard]] char* begin() noexcept;
    [[nodiscard]] const char* begin() const noexcept;

    std::size_t m_read_index{kPrependSize};
    std::size_t m_write_index{kPrependSize};
    std::vector<char> m_buffer;
};

} // namespace rocket
