#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Enable/disable logging in __wrap__open_r (must call before SD file opens). */
void sd_open_trace_set(bool enable);

#ifdef __cplusplus
}
#endif
