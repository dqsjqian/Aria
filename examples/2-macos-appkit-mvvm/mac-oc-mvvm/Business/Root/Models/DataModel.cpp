#include "DataModel.h"

#include <thread>
#include <random>
#include <chrono>
#include <sstream>
#include <ctime>

void DataModel::fetchData(const std::string& tag, FetchCallback callback) {
    // Spawn a detached thread to simulate network latency
    std::thread([tag, cb = std::move(callback)]() mutable {
        // Sleep ~2 seconds
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 95% success rate
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0, 99);
        bool success = dist(rng) > 5;

        if (success) {
            auto now = std::chrono::system_clock::now();
            auto t   = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf{};
#ifdef _WIN32
            localtime_s(&tm_buf, &t);
#else
            localtime_r(&t, &tm_buf);
#endif
            char time_str[64];
            std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

            std::ostringstream oss;
            oss << "[" << tag << "] Server OK at " << time_str;
            cb(FetchResult{true, oss.str()});
        } else {
            std::ostringstream oss;
            oss << "[" << tag << "] Network error";
            cb(FetchResult{false, oss.str()});
        }
    }).detach();
}

void DataModel::fetchData(FetchCallback callback) {
    fetchData("default", std::move(callback));
}
