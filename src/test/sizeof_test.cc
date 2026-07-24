#include "rocket/common/log.h"
#include <iostream>

int main() {
    using rocket::Logger;
    std::cout << "LogEntry:      " << sizeof(Logger::LogEntry) << " bytes\n";
    std::cout << "kFmtStorageSize: " << Logger::LogEntry::kFmtStorageSize << "\n";
    std::cout << "kPerThreadQueueBytes: " << Logger::kPerThreadQueueBytes << " (" << Logger::kPerThreadQueueBytes/1024 << " KB)\n";
    std::cout << "Entries/queue: " << Logger::kPerThreadQueueBytes / sizeof(Logger::LogEntry) << "\n";
    std::cout << "Per-thread mem: " << Logger::kPerThreadQueueBytes / 1024 << " KB\n";
    std::cout << "string (libc++): " << sizeof(std::string) << " bytes\n";
}
