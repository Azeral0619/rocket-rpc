#include "rocket/net/poller/poller.h"

#if defined(__linux__)
#include "rocket/net/poller/epoll_poller.h"
#elif defined(__APPLE__)
#include "rocket/net/poller/kqueue_poller.h"
#endif

namespace rocket {

std::unique_ptr<Poller> Poller::createDefault() {
#if defined(__linux__)
    return EPollPoller::create();
#elif defined(__APPLE__)
    return KqueuePoller::create();
#else
#error "rocket::Poller: unsupported platform (only Linux and macOS are supported)"
#endif
}

} // namespace rocket
