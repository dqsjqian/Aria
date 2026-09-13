#include "aria/reactive/graph.inl"
#include <atomic>

namespace aria::reactive {
Graph& graph_instance() noexcept {
    static Graph graph;
    return graph;
}

std::shared_ptr<FlushTraceFn>& flush_trace_hook_() noexcept {
    static std::shared_ptr<FlushTraceFn> hook;
    return hook;
}

std::uint64_t Node::next_node_id_() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}
}  // namespace aria::reactive
