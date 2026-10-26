// Thin portability layer for the CPA C ABI plugin (Linux, macOS, Windows).
// Everything the plugin needs beyond ISO C lives here.
#ifndef CODEX_TZ_PLATFORM_H
#define CODEX_TZ_PLATFORM_H

#include <stddef.h>

// Settings must survive a container restart, which rules out two tempting
// anchors. The host copies each plugin into a per-process scratch directory
// (/tmp/cliproxy-pluginhost/pid-N/) before loading it, so the *library* path
// points at throwaway storage. The host's data directory is not discoverable
// from inside a plugin either. What does work is the host executable: CPA keeps
// plugins in <executable dir>/plugins, and that directory is the mounted volume.
#define PLUGIN_CONFIG_NAME "codex-timezone-sync-local.config"
// Directory name the host uses for plugins, relative to its executable.
#define PLUGIN_DIR_NAME "plugins"
// Used only when no writable candidate directory could be found.
#define PLUGIN_CONFIG_FALLBACK "/CLIProxyAPI/data/" PLUGIN_CONFIG_NAME
// Windows accepts either separator, but pathlib and other tools compare
// against the native one, so paths are built with it.
#ifdef _WIN32
#define PLUGIN_PATH_SEPARATOR '\\'
#else
#define PLUGIN_PATH_SEPARATOR '/'
#endif

// getenv that also sees the host process environment on Windows, where the
// MinGW CRT keeps its own copy that the host (Go) never updates.
const char *plugin_getenv(const char *name);
// Resolves the settings file path. Candidates are tried in order and the first
// writable, non-volatile directory wins, so a scratch copy of the library can
// never capture the file. Returns 1 and fills out, 0 when none worked.
int plugin_config_path(char *out, size_t cap);
// Directory holding the loaded library, or 0 when it cannot be resolved. This
// may be the host's temporary copy, so callers must not assume it persists.
int plugin_library_dir(char *out, size_t cap);
// Directory holding the host executable, or 0 when it cannot be resolved.
int plugin_executable_dir(char *out, size_t cap);
// Strips the last path component, leaving the containing directory.
int plugin_dir_of(const char *path, char *out, size_t cap);
// 1 when a probe file could be created and removed inside `dir`.
int plugin_dir_writable(const char *dir);
// 1 when `dir` names an existing directory.
int plugin_dir_exists(const char *dir);
// 1 when `path` sits in storage that is wiped when the host restarts.
int plugin_path_is_volatile(const char *path);
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
