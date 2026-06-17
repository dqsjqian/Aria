#include "SearchVm.h"

namespace showcase::search {

SearchVm::SearchVm(aria::IDelayedScheduler& timer) {
    debounced = aria::debounce(query, std::chrono::milliseconds(300), timer);
    distinct  = aria::distinct_until_changed(*debounced);
    sub_ = distinct->on_changed([this](const std::string& q) {
        if (q.empty()) return;
        ++seq_;
        hits.push_back(std::make_shared<SearchHit>(SearchHit{q, seq_}));
    });
}

}  // namespace showcase::search
