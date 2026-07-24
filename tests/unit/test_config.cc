#include "rocket/common/config.h"
#include <fstream>
#include <gtest/gtest.h>

namespace rocket {
namespace {

class ConfigTest : public testing::Test {
  protected:
    static std::string WriteTempYaml(const std::string& content, const std::string& name) {
        const std::string path = "/tmp/rocket_test_" + name + ".yaml";
        std::ofstream out(path);
        out << content;
        out.close();
        return path;
    }
};

TEST_F(ConfigTest, DefaultsWhenKeysMissing) {
    const auto path = WriteTempYaml("LOG:\n  level: INFO\n", "defaults");
    Config::getInstance().reload(path);
    const auto config = Config::getInstance().getConfig();
    EXPECT_EQ(config->log_level, "INFO");
    // Untouched keys keep defaults.
    EXPECT_EQ(config->port, ConfigData::kDefaultServerPort);
    EXPECT_EQ(config->io_threads, 4);
}

TEST_F(ConfigTest, ParsesServerSection) {
    const auto path = WriteTempYaml("SERVER:\n  port: 23456\n  io_threads: 8\n", "server");
    Config::getInstance().reload(path);
    const auto config = Config::getInstance().getConfig();
    EXPECT_EQ(config->port, 23456);
    EXPECT_EQ(config->io_threads, 8);
}

TEST_F(ConfigTest, ParsesStubs) {
    const auto path = WriteTempYaml(
        "STUBS:\n  order_server:\n    - name: order_server\n      ip: 127.0.0.1\n      port: 12345\n      timeout: 3000\n",
        "stubs");
    Config::getInstance().reload(path);
    const auto config = Config::getInstance().getConfig();
    ASSERT_TRUE(config->rpc_stubs.contains("order_server"));
    const auto& stub = config->rpc_stubs.at("order_server");
    EXPECT_EQ(stub.timeout, 3000);
    EXPECT_EQ(stub.addr->toString(), "127.0.0.1:12345");
}

TEST_F(ConfigTest, SkipsInvalidStubEntries) {
    const auto path = WriteTempYaml(
        "STUBS:\n  bad:\n    - name: bad\n      ip: \"\"\n      port: 0\n", "bad_stubs");
    Config::getInstance().reload(path);
    const auto config = Config::getInstance().getConfig();
    EXPECT_FALSE(config->rpc_stubs.contains("bad"));
}

TEST_F(ConfigTest, ReloadReplacesAtomically) {
    const auto before = Config::getInstance().getConfig();
    const auto path = WriteTempYaml("SERVER:\n  port: 31000\n", "atomic");
    Config::getInstance().reload(path);
    const auto after = Config::getInstance().getConfig();
    EXPECT_NE(before, after);
    EXPECT_EQ(after->port, 31000);
    // Old snapshot remains intact for readers that grabbed it earlier.
    EXPECT_NE(before->port, 31000);
}

TEST_F(ConfigTest, MissingFileKeepsCurrentConfig) {
    const auto before = Config::getInstance().getConfig();
    Config::getInstance().reload("/tmp/rocket_test_no_such_file.yaml");
    const auto after = Config::getInstance().getConfig();
    EXPECT_EQ(before, after);
}

} // namespace
} // namespace rocket
