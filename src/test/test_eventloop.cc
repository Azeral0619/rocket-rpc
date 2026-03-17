#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/io_thread.h"
#include "rocket/net/io_thread_group.h"
#include "rocket/net/timer_event.h"

namespace {
constexpr int kTimerIntervalMs = 1000;
} // namespace

void test_io_thread() {

    // int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    // if (listenfd == -1) {
    //   ROCKET_LOG_ERROR("listenfd = -1");
    //   exit(0);
    // }

    // sockaddr_in addr;
    // memset(&addr, 0, sizeof(addr));

    // addr.sin_port = htons(12310);
    // addr.sin_family = AF_INET;
    // inet_aton("127.0.0.1", &addr.sin_addr);

    // int rt = bind(listenfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    // if (rt != 0) {
    //   ROCKET_LOG_ERROR("bind error");
    //   exit(1);
    // }

    // rt = listen(listenfd, 100);
    // if (rt != 0) {
    //   ROCKET_LOG_ERROR("listen error");
    //   exit(1);
    // }

    // rocket::FdEvent event(listenfd);
    // event.listen(rocket::FdEvent::IN_EVENT, [listenfd](){
    //   sockaddr_in peer_addr;
    //   socklen_t addr_len = sizeof(peer_addr);
    //   memset(&peer_addr, 0, sizeof(peer_addr));
    //   int clientfd = accept(listenfd, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len);

    //   ROCKET_LOG_DEBUG("success get client fd[%d], peer addr: [%s:%d]", clientfd, inet_ntoa(peer_addr.sin_addr),
    //   ntohs(peer_addr.sin_port));

    // });

    // 使用原子变量避免数据竞争
    std::atomic<int> timer_count{0};
    auto timer_event = std::make_shared<rocket::TimerEvent>(
        kTimerIntervalMs, true, [&timer_count]() { ROCKET_LOG_INFO("trigger timer event, count={}", timer_count++); });

    // rocket::IOThread io_thread;

    // io_thread.getEventLoop()->addEpollEvent(&event);
    // io_thread.getEventLoop()->addTimerEvent(timer_event);
    // io_thread.start();

    // io_thread.join();

    rocket::IOThreadGroup io_thread_group(2);

    // 必须先启动线程组，EventLoop 才会被创建
    io_thread_group.start();

    rocket::IOThread* io_thread = io_thread_group.getIOThread();
    // io_thread->getEventLoop()->addEpollEvent(&event);
    io_thread->getEventLoop()->addTimerEvent(timer_event);

    rocket::IOThread* io_thread2 = io_thread_group.getIOThread();
    io_thread2->getEventLoop()->addTimerEvent(timer_event);

    // 让定时器运行一段时间（5秒，触发5次）
    ROCKET_LOG_INFO("Waiting for 5 seconds to let timers fire...");
    std::this_thread::sleep_for(std::chrono::seconds(5));
    ROCKET_LOG_INFO("Stopping IO threads...");

    io_thread_group.join();
}

int main(int argc, char* argv[]) {

    const char* config_file = "/home/azeral/code/rocket-rpc/config/client.yaml";
    if (argc == 2) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        config_file = argv[1];
    }

    rocket::Config::getInstance().reload(config_file);

    test_io_thread();

    // rocket::EventLoop* eventloop = new rocket::EventLoop();

    // int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    // if (listenfd == -1) {
    //   ROCKET_LOG_ERROR("listenfd = -1");
    //   exit(0);
    // }

    // sockaddr_in addr;
    // memset(&addr, 0, sizeof(addr));

    // addr.sin_port = htons(12310);
    // addr.sin_family = AF_INET;
    // inet_aton("127.0.0.1", &addr.sin_addr);

    // int rt = bind(listenfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    // if (rt != 0) {
    //   ROCKET_LOG_ERROR("bind error");
    //   exit(1);
    // }

    // rt = listen(listenfd, 100);
    // if (rt != 0) {
    //   ROCKET_LOG_ERROR("listen error");
    //   exit(1);
    // }

    // rocket::FdEvent event(listenfd);
    // event.listen(rocket::FdEvent::IN_EVENT, [listenfd](){
    //   sockaddr_in peer_addr;
    //   socklen_t addr_len = sizeof(peer_addr);
    //   memset(&peer_addr, 0, sizeof(peer_addr));
    //   int clientfd = accept(listenfd, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len);

    //   ROCKET_LOG_DEBUG("success get client fd[%d], peer addr: [%s:%d]", clientfd, inet_ntoa(peer_addr.sin_addr),
    //   ntohs(peer_addr.sin_port));

    // });
    // eventloop->addEpollEvent(&event);

    // int i = 0;
    // rocket::TimerEvent::s_ptr timer_event = std::make_shared<rocket::TimerEvent>(
    //   1000, true, [&i]() {
    //     ROCKET_LOG_INFO("trigger timer event, count=%d", i++);
    //   }
    // );

    // eventloop->addTimerEvent(timer_event);
    // eventloop->loop();

    return 0;
}