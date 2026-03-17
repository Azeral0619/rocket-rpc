add_rules("mode.debug", "mode.release")

set_languages("cxx23")
set_toolchains("gcc-12")
add_requires("protobuf-cpp", "yaml-cpp")
add_requires("fmt", {configs = {header_only = true}})

target("main")
    set_kind("binary")
    add_files("src/main.cpp|src/main.cc")
    add_files("src/rocket/**.cc")
    add_includedirs("src")
    add_cxflags("-pthread")
    add_ldflags("-pthread")
    add_packages("protobuf-cpp", "yaml-cpp", "fmt")

-- Common source files (exclude main.cpp and test files)
local rocket_common_files = {
    "src/rocket/common/log.cc",
    "src/rocket/common/config.cc",
    "src/rocket/common/runtime.cc",
    "src/rocket/common/msg_id_util.cc"
}

local rocket_net_files = {
    "src/rocket/net/fd_event.cc",
    "src/rocket/net/fd_event_group.cc",
    "src/rocket/net/timer.cc",
    "src/rocket/net/timer_event.cc",
    "src/rocket/net/wakeup_fd_event.cc",
    "src/rocket/net/event_loop.cc",
    "src/rocket/net/io_thread.cc",
    "src/rocket/net/io_thread_group.cc"
}

local rocket_tcp_files = {
    "src/rocket/net/tcp/net_addr.cc",
    "src/rocket/net/tcp/tcp_buffer.cc",
    "src/rocket/net/tcp/tcp_connect.cc",
    "src/rocket/net/tcp/tcp_acceptor.cc",
    "src/rocket/net/tcp/tcp_client.cc",
    "src/rocket/net/tcp/tcp_server.cc"
}

local rocket_coder_files = {
    "src/rocket/net/coder/tinypb_coder.cc"
}

local rocket_rpc_files = {
    "src/rocket/net/rpc/rpc_controller.cc",
    "src/rocket/net/rpc/rpc_dispatcher.cc",
    "src/rocket/net/rpc/rpc_interface.cc",
    "src/rocket/net/rpc/rpc_channel.cc"
}

target("test_log")
    set_kind("binary")
    add_files("src/test/test_log.cc")
    add_files(rocket_common_files)
    add_files("src/rocket/net/tcp/net_addr.cc")
    add_includedirs("src")
    add_cxflags("-pthread")
    add_ldflags("-pthread")
    add_packages("yaml-cpp", "fmt")

target("test_tcp")
    set_kind("binary")
    add_files("src/test/test_tcp.cc")
    add_files(rocket_common_files)
    add_files(rocket_net_files)
    add_files(rocket_tcp_files)
    add_files(rocket_coder_files)
    add_files(rocket_rpc_files)
    add_includedirs("src")
    add_cxflags("-pthread")
    add_ldflags("-pthread")
    add_packages("protobuf-cpp", "yaml-cpp", "fmt")

target("test_client")
    set_kind("binary")
    add_files("src/test/test_client.cc")
    add_files(rocket_common_files)
    add_files(rocket_net_files)
    add_files(rocket_tcp_files)
    add_files(rocket_coder_files)
    add_files(rocket_rpc_files)
    add_includedirs("src")
    add_cxflags("-pthread")
    add_ldflags("-pthread")
    add_packages("protobuf-cpp", "yaml-cpp", "fmt")

target("test_eventloop")
    set_kind("binary")
    add_files("src/test/test_eventloop.cc")
    add_files(rocket_common_files)
    add_files(rocket_net_files)
    add_files("src/rocket/net/tcp/net_addr.cc")
    add_includedirs("src")
    add_cxflags("-pthread")
    add_ldflags("-pthread")
    add_packages("yaml-cpp", "fmt")

target("test_rpc_client")
    set_kind("binary")
    add_files("src/test/test_rpc_client.cc")
    add_files("src/test/order.pb.cc")
    add_files(rocket_common_files)
    add_files(rocket_net_files)
    add_files(rocket_tcp_files)
    add_files(rocket_coder_files)
    add_files(rocket_rpc_files)
    add_includedirs("src")
    add_cxflags("-pthread")
    add_ldflags("-pthread")
    add_packages("protobuf-cpp", "yaml-cpp", "fmt")

target("test_rpc_server")
    set_kind("binary")
    add_files("src/test/test_rpc_server.cc")
    add_files("src/test/order.pb.cc")
    add_files(rocket_common_files)
    add_files(rocket_net_files)
    add_files(rocket_tcp_files)
    add_files(rocket_coder_files)
    add_files(rocket_rpc_files)
    add_includedirs("src")
    add_cxflags("-pthread")
    add_ldflags("-pthread")
    add_packages("protobuf-cpp", "yaml-cpp", "fmt")