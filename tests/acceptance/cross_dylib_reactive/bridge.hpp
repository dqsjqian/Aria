#pragma once
#include <aria/abi/export.hpp>
namespace aria { class IProperty; }
#ifdef ARIA_TEST_BRIDGE_BUILD
#  define ARIA_TEST_BRIDGE_API ARIA_EXPORT
#else
#  define ARIA_TEST_BRIDGE_API ARIA_IMPORT
#endif
extern "C" {
ARIA_TEST_BRIDGE_API void* aria_test_graph_address();
ARIA_TEST_BRIDGE_API void* aria_test_projection_create(aria::IProperty* source,
                                                       int* value, int* changes);
ARIA_TEST_BRIDGE_API void aria_test_projection_destroy(void* projection);
ARIA_TEST_BRIDGE_API void aria_test_publish_trace();
ARIA_TEST_BRIDGE_API void* aria_test_flush_hook_address();
ARIA_TEST_BRIDGE_API const char* aria_test_source_label();
}
