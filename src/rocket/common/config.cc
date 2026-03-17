#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/tcp/net_addr.h"

#include <atomic>
#include <exception>
#include <memory>
#include <string_view>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/node/parse.h>
#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace rocket {
namespace {
constexpr int kMaxPort = 65535;

int AsIntOrDefault(const YAML::Node& node, std::string_view key, int default_value) {
    if (!node || !node[key]) {
        return default_value;
    }
    return node[key].as<int>();
}

std::string AsStringOrDefault(const YAML::Node& node, std::string_view key, const std::string& default_value) {
    if (!node || !node[key]) {
        return default_value;
    }
    return node[key].as<std::string>();
}
} // namespace

Config::Config() : m_config(std::make_shared<ConfigData>()) {}

void Config::reload(std::string_view yamlfile) {
    if (yamlfile.empty()) {
        return;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(std::string(yamlfile));
    } catch (const std::exception& e) {
        std::cerr << "[Config] failed to load yaml file: " << yamlfile << ", error: " << e.what() << '\n';
        return;
    } catch (...) {
        std::cerr << "[Config] failed to load yaml file: " << yamlfile << ", unknown error\n";
        return;
    }

    auto new_config = std::make_shared<ConfigData>();

    const YAML::Node log_node = root["LOG"];
    new_config->log_level = AsStringOrDefault(log_node, "level", new_config->log_level);
    new_config->log_file_name = AsStringOrDefault(log_node, "file_name", new_config->log_file_name);
    new_config->log_file_path = AsStringOrDefault(log_node, "file_path", new_config->log_file_path);
    new_config->log_max_file_size = AsIntOrDefault(log_node, "max_file_size", new_config->log_max_file_size);
    new_config->log_sync_interval = AsIntOrDefault(log_node, "sync_interval", new_config->log_sync_interval);
    new_config->log_queue_capacity = AsIntOrDefault(log_node, "queue_capacity", new_config->log_queue_capacity);

    const YAML::Node server_node = root["SERVER"];
    new_config->port = AsIntOrDefault(server_node, "port", new_config->port);
    new_config->io_threads = AsIntOrDefault(server_node, "io_threads", new_config->io_threads);

    const YAML::Node stubs_node = root["STUBS"];
    if (stubs_node && stubs_node.IsMap()) {
        for (auto it = stubs_node.begin(); it != stubs_node.end(); ++it) {
            const auto service_name = it->first.as<std::string>();
            const YAML::Node service_stubs = it->second;
            if (!service_stubs || !service_stubs.IsSequence()) {
                continue;
            }

            for (const auto& stub_node : service_stubs) {
                if (!stub_node || !stub_node.IsMap()) {
                    continue;
                }

                RpcStub stub;
                stub.name = AsStringOrDefault(stub_node, "name", service_name);
                stub.timeout = AsIntOrDefault(stub_node, "timeout", RpcStub::kDefaultTimeout);

                const std::string ip = AsStringOrDefault(stub_node, "ip", "");
                const int port = AsIntOrDefault(stub_node, "port", 0);
                if (ip.empty() || port <= 0 || port > kMaxPort) {
                    continue;
                }

                stub.addr = std::make_shared<IPNetAddr>(ip, static_cast<uint16_t>(port));
                new_config->rpc_stubs[stub.name] = std::move(stub);
            }
        }
    }
    m_config.store(new_config, std::memory_order_release);
    Logger::getInstance().reloadFromConfig();
}

} // namespace rocket
