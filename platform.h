// Thin portability layer for the CPA C ABI plugin (Linux, macOS, Windows).
// Everything the plugin needs beyond ISO C lives here.
#ifndef CODEX_TZ_PLATFORM_H
#define CODEX_TZ_PLATFORM_H

#include <stddef.h>

// Default data directory inside the CPA container; the env var overrides it.
#define PLUGIN_DEFAULT_CONFIG "/CLIProxyAPI/data/codex-timezone-sync.config"
#define PLUGIN_DEFAULT_EVENTS "/CLIProxyAPI/data/codex-timezone-sync.events"

// getenv that also sees the host process environment on Windows, where the
// MinGW CRT keeps its own copy that the host (Go) never updates.
const char *plugin_getenv(const char *name);
// Current wall-clock time in Unix seconds.
long long plugin_now(void);
// Atomic, fsync'd, 0600 write through a temporary file in the same directory.
int plugin_write_file_atomic(const char *path, const char *data, size_t length);

#ifdef _WIN32
#include <windows.h>
typedef volatile LONG plugin_mutex_t;
#define PLUGIN_MUTEX_INITIALIZER 0
static inline void plugin_mutex_lock(plugin_mutex_t *mutex) {
  while (InterlockedCompareExchange(mutex, 1, 0) != 0) Sleep(0);
}
static inline void plugin_mutex_unlock(plugin_mutex_t *mutex) {
  InterlockedExchange(mutex, 0);
}
#else
#include <pthread.h>
typedef pthread_mutex_t plugin_mutex_t;
#define PLUGIN_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static inline void plugin_mutex_lock(plugin_mutex_t *mutex) { pthread_mutex_lock(mutex); }
static inline void plugin_mutex_unlock(plugin_mutex_t *mutex) { pthread_mutex_unlock(mutex); }
#endif

#endif
