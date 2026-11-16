#pragma once

#include "aria/abi/export.hpp"

#if defined(ARIA_STATIC_LINKAGE) || defined(ARIA_I18N_STATIC)
#  define ARIA_I18N_API
#elif defined(ARIA_I18N_BUILD)
#  define ARIA_I18N_API ARIA_EXPORT
#else
#  define ARIA_I18N_API ARIA_IMPORT
#endif
