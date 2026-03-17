#pragma once

namespace rocket {

template <typename T>
class Singleton {
  public:
    [[nodiscard]] static T& getInstance() noexcept {
        static T instance;
        return instance;
    }

    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton(Singleton&&) = delete;
    Singleton& operator=(Singleton&&) = delete;

  protected:
    Singleton() = default;
    virtual ~Singleton() = default;
};

} // namespace rocket
