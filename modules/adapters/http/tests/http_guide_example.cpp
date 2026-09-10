#include "aria/adapters/http/http_adapter.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/runtime/dispatcher.hpp"
#include <memory>
#include <string>

int main() {
    aria::adapters::http::HttpAdapterConfig config;
    config.port = 0;
    config.worker_threads = 4;
    auto http = std::make_shared<aria::adapters::http::HttpAdapter>(config);
    auto dispatcher = std::make_shared<aria::runtime::SimpleDispatcher>();
    aria::Property<std::string> query("hello");
    aria::binding::BindingEngine engine(http, dispatcher,
        aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);
    auto& search = http->register_view("search_query", "text");
    engine.bind_text(query, search);
    if (!http->start()) return 1;
    // A real host repeatedly pumps this dispatcher on the graph owner thread.
    dispatcher->pump();
    http->stop();
    return http->actual_port() == 0 ? 0 : 1;
}
