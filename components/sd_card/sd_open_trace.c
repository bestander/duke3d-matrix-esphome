/*
 * Link with -Wl,--wrap=_open_r so every path open goes through here before esp_vfs.
 * Logs read-capable opens under /sdcard (fopen "r"/"rb"/"r+" etc. use O_RDONLY or O_RDWR).
 */
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/reent.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sd_open_trace.h"

static const char *TAG = "sd_open";

static bool g_sd_open_trace;

void sd_open_trace_set(bool enable) { g_sd_open_trace = enable; }

extern int __real__open_r(struct _reent *r, const char *path, int flags, int mode);

static bool path_under_sd(const char *path) {
  return path != NULL && strncmp(path, "/sdcard", 7) == 0;
}

static bool wants_read(int flags) { return (flags & O_ACCMODE) != O_WRONLY; }

int __wrap__open_r(struct _reent *r, const char *path, int flags, int mode) {
  const bool log_this =
      g_sd_open_trace && path_under_sd(path) && wants_read(flags);

  if (log_this) {
    const char *tn = pcTaskGetName(NULL);
    ESP_LOGI(TAG, "read-open path=%s flags=0x%x task=%s", path, (unsigned) flags,
             tn != NULL ? tn : "?");
  }

  const int fd = __real__open_r(r, path, flags, mode);

  if (log_this && fd < 0) {
    ESP_LOGW(TAG, "read-open FAILED path=%s -> fd=%d errno=%d", path, fd, r->_errno);
  }

  return fd;
}
