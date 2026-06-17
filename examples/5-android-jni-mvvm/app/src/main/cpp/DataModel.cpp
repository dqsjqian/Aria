#include "DataModel.h"
#include <chrono>
#include <thread>
#include <random>
#include <android/log.h>

static const char* TAG = "DataModel";

void DataModel::fetchData(const std::string& tag, FetchCallback callback) {
    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Fetching data for tag: %s", tag.c_str());

    // Simulate async fetch on a detached worker thread
    // (mirrors iOS demo3's DataModel.cpp)
    std::thread([tag, callback]() {
        // Simulate ~2s delay
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 95% success rate
        std::random_device rd;
        std::mt19937 gen(rd());
        std::bernoulli_distribution d(0.95);
        bool ok = d(gen);

        FetchResult result;
        result.ok = ok;
        if (ok) {
            result.message = "Fetch succeeded for tag: " + tag;
        } else {
            result.message = "Fetch failed for tag: " + tag;
        }

        // Callback fires on the background thread.
        // ViewModel marshals back to the UI thread.
        callback(result);
    }).detach();
}

void DataModel::fetchData(FetchCallback callback) {
    fetchData("default", callback);
}
