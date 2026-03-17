#include <google/protobuf/service.h>
#include <memory>
#include <string>

#include "order.pb.h"
#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/common/msg_id_util.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/rpc/rpc_channel.h"
#include "rocket/net/rpc/rpc_closure.h"
#include "rocket/net/tcp/net_addr.h"

namespace {
constexpr int kTestPrice = 100;
} // namespace

void test_rpc_channel() {

    rocket::EventLoop event_loop;

    auto request = rocket::NewMessage<makeOrderRequest>();
    auto response = rocket::NewMessage<makeOrderResponse>();

    request->set_price(kTestPrice);
    request->set_goods("apple");

    const auto& config = rocket::Config::getInstance().getConfig();
    const auto& stub = config->rpc_stubs.at("order_server");

    auto controller = rocket::NewRpcController();
    controller->SetMsgId(rocket::MsgIDUtil::GenMsgID());
    controller->SetTimeout(stub.timeout);

    auto channel = rocket::NewRpcChannel(stub.addr->toString());

    auto closure =
        std::make_shared<rocket::RpcClosure>(nullptr, [request, response, channel, controller, &event_loop]() mutable {
            if (controller->GetErrorCode() == 0) {
                ROCKET_LOG_INFO("call rpc success, request[{}], response[{}]", request->ShortDebugString(),
                                response->ShortDebugString());
            } else {
                ROCKET_LOG_ERROR("call rpc failed, request[{}], error code[{}], error info[{}]",
                                 request->ShortDebugString(), controller->GetErrorCode(), controller->GetErrorInfo());
            }

            channel.reset();
            event_loop.stop();
        });

    channel->Init(controller, request, response, closure);
    Order_Stub(channel.get()).makeOrder(controller.get(), request.get(), response.get(), closure.get());

    event_loop.loop();
}

int main(int argc, char* argv[]) {

    const char* config_file = "/home/azeral/code/rocket-rpc/config/client.yaml";
    if (argc == 2) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        config_file = argv[1];
    }

    rocket::Config::getInstance().reload(config_file);

    // test_tcp_client();
    test_rpc_channel();

    return 0;
}