#include "MainViewModel.hpp"

#include <android/log.h>

static const char* TAG = "MainViewModel";

MainViewModel::MainViewModel(std::shared_ptr<DataModel> model)
    : model_(std::move(model)),
      fetch_cmd([this](const std::string& tag) { doFetch(tag); }),
      dismiss_toast([this]() { toast_message.set(""); })
{}

void MainViewModel::doFetch(const std::string& tag) {
    is_loading.set(true);
    status_text.set("Fetching (" + tag + ")...");
    result_text.set("");

    auto ws = weak_self();

    model_->fetchData(tag, [ws, tag](FetchResult result) {
        auto self = ws.lock();
        if (!self) return;  // ViewModel destroyed

        auto apply = [self, result, tag]() {
            self->is_loading.set(false);
            if (result.ok) {
                self->result_text.set(result.message);
                self->status_text.set(tag + " fetch completed");
            } else {
                self->result_text.set(result.message);
                self->status_text.set(tag + " fetch failed");
            }
            self->toast_message.set(tag + ": " + result.message);
        };

        // Dispatch to main thread if available
        if (self->main_dispatcher) {
            self->main_dispatcher(std::move(apply));
        } else {
            apply();  // Fallback: apply on current thread
        }
    });
}
