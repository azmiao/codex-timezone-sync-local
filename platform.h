// Thin portability layer for the CPA C ABI plugin (Linux, macOS, Windows).
// Everything the plugin needs beyond ISO C lives here.
#ifndef CODEX_TZ_PLATFORM_H
#define CODEX_TZ_PLATFORM_H

#include <stddef.h>

// Settings live next to the plugin binary, because that directory is the one
// deployments actually mount as a volume. Hard-coding a container path such as
// /CLIProxyAPI/data loses the file whenever the container is recreated.
#define PLUGIN_CONFIG_NAME "codex-timezone-sync-local.config"
// Used only when the library path cannot be resolved.
#define PLUGIN_CONFIG_FALLBACK "/CLIProxyAPI/data/" PLUGIN_CONFIG_NAME

// getenv that also sees the host process environment on Windows, where the
// MinGW CRT keeps its own copy that the host (Go) never updates.
const char *plugin_getenv(const char *name);
// Resolves the settings file path: env override, else next to the library.
// Returns 1 and fills out on success, 0 when no path could be produced.
int plugin_config_path(char *out, size_t cap);
// Directory holding the loaded library, or 0 when it cannot be resolved.
int plugin_library_dir(char *out, size_t cap);
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
