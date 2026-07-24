#include "rocket/common/log.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
size_t getRSS() {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
}
#elif defined(__linux__)
#include <fstream>
size_t getRSS() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.starts_with("VmRSS:"))
            return std::stoull(line.substr(6)) * 1024;
    return 0;
}
#endif

void printMem(const char* label) {
    size_t rss = getRSS();
    printf("%-30s %6zu KB  (%5.1f MB)\n", label, rss/1024, rss/1048576.0);
}

int main() {
    std::filesystem::create_directories("./bench_logs");

    printMem("startup");

    auto& logger = rocket::Logger::getInstance();
    printMem("after getInstance");

    rocket::Logger::Options opts;
    opts.file_path = "./bench_logs/mem.log";
    opts.per_thread_queue_bytes = 256 * 1024; // default
    logger.start(opts);
    printMem("after logger.start()");

    // Log from main thread => creates 1 slot queue
    for (int i = 0; i < 1000; ++i)
        ROCKET_LOG_INFO("init {}", i);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printMem("1 thread, 1000 logs");

    // Spin up 4 threads logging
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t]() {
            int i = 0;
            while (!stop.load()) {
                ROCKET_LOG_INFO("worker {} msg {}", t, i++);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    printMem("5 threads logging (1+4)");

    stop.store(true);
    for (auto& w : workers) w.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    printMem("after workers stopped");

    logger.stop();
    printMem("after logger.stop()");
}
