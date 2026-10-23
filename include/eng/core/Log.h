#pragma once

#include <chrono>
#include <ctime>
#include <iostream>
#include <string>

namespace eng {

inline std::string logTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[9];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

}  // namespace eng

// Timestamped line to stdout — which crashlog::init() tees into the log
// file, so every ENG_LOG survives a crash.
#define ENG_LOG(msg) std::cout << "[" << eng::logTimestamp() << "] " << msg << "\n"
