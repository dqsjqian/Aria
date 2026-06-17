#pragma once

/// DataModel — C++ model layer.
/// Simulates an async data fetch (~2-second delay) with a 95% success rate.
/// Runs the fetch on a detached worker thread; the callback fires on that
/// thread — the ViewModel is responsible for marshalling back to the UI thread.

#include <functional>
#include <string>

struct FetchResult {
    bool ok;
    std::string message;   // result on success, error description on failure
};

using FetchCallback = std::function<void(FetchResult)>;

class DataModel {
public:
    DataModel() = default;
    ~DataModel() = default;

    DataModel(const DataModel&) = delete;
    DataModel& operator=(const DataModel&) = delete;

    /// Fetch data from "server". Callback fires on a background thread after ~2s.
    void fetchData(const std::string& tag, FetchCallback callback);

    /// Convenience: fetch with default tag.
    void fetchData(FetchCallback callback);
};
