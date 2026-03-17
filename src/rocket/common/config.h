#pragma once

#include "rocket/common/singleton.h"
#include "rocket/net/tcp/net_addr.h"
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rocket {

struct RpcStub {
    static constexpr int kDefaultTimeout = 2000; // ms
    std::string name;
    NetAddr::s_ptr addr;
    int timeout{kDefaultTimeout};
};
struct StringTransparentHasher {
    using is_transparent = void;
    size_t operator()(const std::string& key) const { return std::hash<std::string>{}(key); }
    size_t operator()(std::string_view key) const { return std::hash<std::string_view>{}(key); }
    size_t operator()(const char* key) const { return std::hash<std::string_view>{}(key); }
};

struct ConfigData {
    static constexpr int kDefaultLogMaxFileSize = 1024 * 1024 * 1024; // 1GB
    static constexpr int kDefaultLogSyncInterval = 500;               // ms
    static constexpr int kDefaultLogQueueCapacity = 1 << 14;
    static constexpr int kDefaultServerPort = 12345;

    std::string log_level{"DEBUG"};
    std::string log_file_name{"rocket_rpc.log"};
    std::string log_file_path{"../log"};
    int log_max_file_size{kDefaultLogMaxFileSize};
    int log_sync_interval{kDefaultLogSyncInterval};
    int log_queue_capacity{kDefaultLogQueueCapacity};

    int port{kDefaultServerPort};
    int io_threads{4};

    std::unordered_map<std::string, RpcStub, StringTransparentHasher, std::equal_to<>> rpc_stubs;
};

class Config final : public Singleton<Config> {
  public:
    void reload(std::string_view yamlfile);

    [[nodiscard]] std::shared_ptr<const ConfigData> getConfig() const noexcept {
        return m_config.load(std::memory_order_acquire);
    }

  private:
    friend class Singleton<Config>;

    Config();

    std::atomic<std::shared_ptr<ConfigData>> m_config;
};

} // namespace rocket