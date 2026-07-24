#include "rocket/net/tcp/net_addr.h"
#include <gtest/gtest.h>

namespace rocket {
namespace {

TEST(NetAddr, ParseIpPortString) {
    IPNetAddr addr("127.0.0.1:8080");
    EXPECT_TRUE(addr.checkValid());
    EXPECT_EQ(addr.toString(), "127.0.0.1:8080");
    EXPECT_EQ(addr.getFamily(), AF_INET);
}

TEST(NetAddr, ConstructFromIpAndPort) {
    IPNetAddr addr("10.0.0.1", 9090);
    EXPECT_TRUE(addr.checkValid());
    EXPECT_EQ(addr.toString(), "10.0.0.1:9090");
}

TEST(NetAddr, CheckValidRejectsGarbage) {
    EXPECT_FALSE(IPNetAddr::checkValid("not-an-address"));
    EXPECT_FALSE(IPNetAddr::checkValid("127.0.0.1"));
    EXPECT_FALSE(IPNetAddr::checkValid("999.1.1.1:80"));
    EXPECT_FALSE(IPNetAddr::checkValid(""));
    EXPECT_TRUE(IPNetAddr::checkValid("192.168.1.1:65535"));
}

TEST(NetAddr, SockaddrRoundtrip) {
    IPNetAddr addr("127.0.0.1:12345");
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(addr.getSockAddr());
    IPNetAddr copy(*in);
    EXPECT_EQ(copy.toString(), addr.toString());
}

} // namespace
} // namespace rocket
