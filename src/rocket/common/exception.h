#pragma once
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace rocket {

class RocketException : public std::exception {
  public:
    RocketException(std::int32_t error_code, std::string error_info)
        : m_error_code(error_code), m_error_info(std::move(error_info)) {}

    RocketException(const RocketException&) = default;
    RocketException& operator=(const RocketException&) = default;
    RocketException(RocketException&&) noexcept = default;
    RocketException& operator=(RocketException&&) noexcept = default;

    virtual void handle() = 0;

    ~RocketException() override = default;

    [[nodiscard]] std::int32_t errorCode() const noexcept { return m_error_code; }

    [[nodiscard]] std::string_view errorInfo() const noexcept { return m_error_info; }

    [[nodiscard]] const char* what() const noexcept override { return m_error_info.c_str(); }

  private:
    std::int32_t m_error_code{0};
    std::string m_error_info;
};

} // namespace rocket
