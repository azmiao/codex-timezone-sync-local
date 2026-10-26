// Portability layer implementation. No third-party dependencies.
// Feature macros must be defined before any libc header so that mkstemp and
// friends are declared under -std=c11. dlfcn.h only exposes Dl_info and dladdr
// when _GNU_SOURCE (or _DEFAULT_SOURCE) is also set, so _POSIX_C_SOURCE alone
// is not enough on glibc.
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#elif !defined(_WIN32)
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

// macOS resolves the host executable through _NSGetExecutablePath.
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

// The host process is a Go binary linked against the UCRT, while the plugin may
// use a different CRT; read the real process environment instead of the copy.
// Results rotate through a small pool so several variables can be read within
// one expression without overwriting each other. Requests are handled
// concurrently, so the pool cursor is guarded.
const char *plugin_getenv(const char *name) {
  enum { SLOTS = 8, CAP = 1024 };
  static char values[SLOTS][CAP];
  static int next = 0;
  static plugin_mutex_t mutex = PLUGIN_MUTEX_INITIALIZER;
  if (!name || !*name) return NULL;
  plugin_mutex_lock(&mutex);
  char *slot = values[next];
  next = (next + 1) % SLOTS;
  DWORD length = GetEnvironmentVariableA(name, slot, CAP);
  plugin_mutex_unlock(&mutex);
  return length > 0 && length < CAP ? slot : NULL;
}

long long plugin_now(void) {
  FILETIME file_time;
  GetSystemTimeAsFileTime(&file_time);
  ULARGE_INTEGER ticks;
  ticks.LowPart = file_time.dwLowDateTime;
  ticks.HighPart = file_time.dwHighDateTime;
  return (long long)((ticks.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

int plugin_write_file_atomic(const char *path, const char *data, size_t length) {
  size_t capacity = strlen(path) + 8;
  char *temp = (char *)malloc(capacity);
  if (!temp) return 0;
  snprintf(temp, capacity, "%s.tmp", path);
  int fd = _open(temp, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
  int ok = fd >= 0;
  if (ok) {
    size_t written = 0;
    while (written < length) {
      int chunk = _write(fd, data + written, (unsigned)(length - written));
      if (chunk <= 0) { ok = 0; break; }
      written += (size_t)chunk;
    }
    if (ok) ok = _commit(fd) == 0;
    _close(fd);
  }
  if (ok) ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok) DeleteFileA(temp);
  free(temp);
  return ok;
}

#else

#include <sys/stat.h>
#include <unistd.h>

const char *plugin_getenv(const char *name) {
  return getenv(name);
}

long long plugin_now(void) {
  return (long long)time(NULL);
}

int plugin_write_file_atomic(const char *path, const char *data, size_t length) {
  size_t capacity = strlen(path) + 8;
  char *temp = (char *)malloc(capacity);
  if (!temp) return 0;
  snprintf(temp, capacity, "%s.XXXXXX", path);
  int fd = mkstemp(temp);
  if (fd < 0) { free(temp); return 0; }
  // mkstemp creates the file 0600 already; chmod by path keeps the intent
  // explicit without depending on fchmod's feature-test visibility.
  int ok = chmod(temp, 0600) == 0;
  size_t written = 0;
  while (ok && written < length) {
    ssize_t chunk = write(fd, data + written, length - written);
    if (chunk <= 0) { ok = 0; break; }
    written += (size_t)chunk;
  }
  if (ok) ok = fsync(fd) == 0;
  if (close(fd) != 0) ok = 0;
  if (ok) ok = rename(temp, path) == 0;
  if (!ok) unlink(temp);
  free(temp);
  return ok;
}

#endif

// ---------------------------------------------------------------------------
// Settings path resolution
// ---------------------------------------------------------------------------
// The plugin has no API to learn where the host stores its data, and the two
// obvious anchors both fail. CPA copies every plugin into a per-process scratch
// directory (/tmp/cliproxy-pluginhost/pid-N/) before loading it, so the library
// path points at storage that is wiped on restart — that was the original bug.
// The host executable is the reliable anchor instead: CPA keeps its plugins in
// <executable dir>/plugins, which operators mount as a volume, so a file written
// there survives container recreation.

#define PATH_BUF 1200

// Strips the trailing path component, leaving the containing directory.
int plugin_dir_of(const char *path, char *out, size_t cap) {
  if (!path || !*path || !out || cap < 2) return 0;
  size_t length = strlen(path);
  while (length > 0 && (path[length - 1] == '/' || path[length - 1] == '\\')) length--;
  while (length > 0 && path[length - 1] != '/' && path[length - 1] != '\\') length--;
  while (length > 1 && (path[length - 1] == '/' || path[length - 1] == '\\')) length--;
  if (length == 0 || length >= cap) return 0;
  memcpy(out, path, length);
  out[length] = '\0';
  return 1;
}

// Joins a directory and a name, rejecting anything that would not fit.
static int join_path(const char *dir, const char *name, char *out, size_t cap) {
  if (!dir || !name || !out || cap < 2) return 0;
  size_t dir_length = strlen(dir);
  size_t name_length = strlen(name);
  // One separator, the name, and the terminator must all fit.
  if (dir_length + 1 + name_length + 1 > cap) return 0;
  // Assembled by hand: snprintf cannot prove the bound above to the compiler,
  // which trips -Werror=format-truncation on some targets.
  memcpy(out, dir, dir_length);
  // Windows callers (and Python's pathlib) expect the native separator.
  out[dir_length] = PLUGIN_PATH_SEPARATOR;
  memcpy(out + dir_length + 1, name, name_length);
  out[dir_length + 1 + name_length] = '\0';
  return 1;
}

static int has_prefix(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

int plugin_path_is_volatile(const char *path) {
  if (!path || !*path) return 1;
  // The host's scratch copies and the usual tmpfs mounts. A file written here
  // appears to work and then vanishes on the next start. Prefixes carry the
  // trailing separator so a directory such as /tmpdata is not caught.
  static const char *const scratch[] = {
    // /var/folders is the macOS per-user temp root.
    "/tmp/", "/var/tmp/", "/var/folders/", "/dev/shm/", "/run/user/",
  };
  for (size_t i = 0; i < sizeof(scratch) / sizeof(scratch[0]); i++)
    if (has_prefix(path, scratch[i])) return 1;
  // The mount points themselves.
  static const char *const roots[] = {"/tmp", "/var/tmp", "/dev/shm"};
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
    if (strcmp(path, roots[i]) == 0) return 1;
#ifdef _WIN32
  // Windows temp storage plays the same role for the copied DLL.
  const char *temp = plugin_getenv("TEMP");
  if (temp && *temp) {
    size_t length = strlen(temp);
    if (_strnicmp(path, temp, length) == 0 && (path[length] == '\\' || path[length] == '/'))
      return 1;
  }
#endif
  return 0;
}

#ifdef _WIN32

static int file_exists(const char *path) {
  DWORD attributes = GetFileAttributesA(path);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int plugin_dir_writable(const char *dir) {
  // The probe name carries the process id so concurrent instances never
  // collide; FILE_FLAG_DELETE_ON_CLOSE removes it when the handle closes.
  char name[64];
  snprintf(name, sizeof(name), ".codex-tz-probe-%lu", (unsigned long)GetCurrentProcessId());
  char probe[PATH_BUF];
  if (!join_path(dir, name, probe, sizeof(probe))) return 0;
  HANDLE handle = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (handle == INVALID_HANDLE_VALUE) return 0;
  CloseHandle(handle);
  return 1;
}

int plugin_dir_exists(const char *dir) {
  DWORD attributes = GetFileAttributesA(dir);
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int plugin_library_dir(char *out, size_t cap) {
  // GetModuleFileNameA inside a DLL reports the DLL path, not the host EXE.
  HMODULE module = NULL;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)(const void *)&plugin_library_dir, &module))
    return 0;
  char path[MAX_PATH];
  DWORD length = GetModuleFileNameA(module, path, (DWORD)sizeof(path));
  if (length == 0 || length >= sizeof(path)) return 0;
  path[length] = '\0';
  return plugin_dir_of(path, out, cap);
}

int plugin_executable_dir(char *out, size_t cap) {
  char path[MAX_PATH];
  DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
  if (length == 0 || length >= sizeof(path)) return 0;
  path[length] = '\0';
  return plugin_dir_of(path, out, cap);
}

#else

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static int file_exists(const char *path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

int plugin_dir_writable(const char *dir) {
  // The probe name carries the process id so concurrent instances never
  // collide, and it is removed immediately either way.
  char name[64];
  snprintf(name, sizeof(name), ".codex-tz-probe-%ld", (long)getpid());
  char probe[PATH_BUF];
  if (!join_path(dir, name, probe, sizeof(probe))) return 0;
  int fd = open(probe, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  if (fd < 0) return 0;
  close(fd);
  unlink(probe);
  return 1;
}

int plugin_dir_exists(const char *dir) {
  struct stat info;
  return stat(dir, &info) == 0 && S_ISDIR(info.st_mode);
}

// Linux and macOS both resolve the library path through dladdr, which reports
// the host's temporary copy rather than the installed location.
int plugin_library_dir(char *out, size_t cap) {
  Dl_info info;
  if (!dladdr((const void *)&plugin_library_dir, &info) || !info.dli_fname) return 0;
  return plugin_dir_of(info.dli_fname, out, cap);
}

int plugin_executable_dir(char *out, size_t cap) {
#if defined(__APPLE__)
  char raw[PATH_BUF * 2];
  uint32_t size = (uint32_t)sizeof(raw);
  if (_NSGetExecutablePath(raw, &size) != 0) return 0;
  char real[PATH_BUF * 2];
  // The reported path may contain symlinks; resolve when possible.
  if (!realpath(raw, real)) snprintf(real, sizeof(real), "%s", raw);
  return plugin_dir_of(real, out, cap);
#else
  char raw[PATH_BUF];
  ssize_t length = readlink("/proc/self/exe", raw, sizeof(raw) - 1);
  if (length <= 0) return 0;
  raw[length] = '\0';
  return plugin_dir_of(raw, out, cap);
#endif
}

#endif

// Candidate directories, most durable first. The environment overrides are
// handled by the caller before this runs.
static int candidate_dirs(char dirs[][PATH_BUF], int max) {
  int count = 0;
  char exe[PATH_BUF];
  if (plugin_executable_dir(exe, sizeof(exe))) {
    // <exe dir>/plugins is where CPA installs plugins and the one directory
    // deployments mount as a volume.
    if (count < max && join_path(exe, PLUGIN_DIR_NAME, dirs[count], PATH_BUF)) count++;
    // The host data directory, in case plugins live elsewhere.
    if (count < max && join_path(exe, "data", dirs[count], PATH_BUF)) count++;
  }

  // The library directory only helps when the host loads the plugin in place;
  // its scratch copy is rejected by the volatility check below.
  char lib[PATH_BUF];
  if (plugin_library_dir(lib, sizeof(lib)) && count < max)
    snprintf(dirs[count++], PATH_BUF, "%s", lib);

  // A host that loads plugins from a helper process started in scratch storage
  // reports /tmp as its executable directory. The official container layout is
  // then the only durable anchor left, so try it before per-user locations.
  static const char *const container[] = {
    "/CLIProxyAPI/plugins", "/CLIProxyAPI/data",
  };
  for (size_t i = 0; i < sizeof(container) / sizeof(container[0]) && count < max; i++)
    snprintf(dirs[count++], PATH_BUF, "%s", container[i]);

  // Per-user fallback for a host that runs without a plugins volume.
  const char *config_home = plugin_getenv("XDG_CONFIG_HOME");
  if (config_home && *config_home && count < max)
    snprintf(dirs[count++], PATH_BUF, "%s", config_home);
  const char *home = plugin_getenv("HOME");
  if (home && *home && count < max && join_path(home, ".config", dirs[count], PATH_BUF))
    count++;
  return count;
}

// Only existing directories are considered: creating one at a guessed path
// would scatter empty folders through the host's install tree.
static int usable_dir(const char *dir) {
  return !plugin_path_is_volatile(dir) && plugin_dir_exists(dir);
}

// Joins a directory and the config name, checking that the result fits.
static int config_in(const char *dir, char *out, size_t cap) {
  if (!join_path(dir, PLUGIN_CONFIG_NAME, out, cap)) return 0;
  return strlen(out) < cap;
}

// Last resort: keep settings working for this process even though they will not
// survive a restart. The library directory always exists, and the panel warns
// when the resolved path is volatile.
static void fallback_path(char *out, size_t cap) {
  char lib[PATH_BUF];
  if (plugin_library_dir(lib, sizeof(lib)) && config_in(lib, out, cap)) return;
  snprintf(out, cap, "%s", PLUGIN_CONFIG_FALLBACK);
}

// Scans the candidate list once and caches the winner. Probing each directory
// costs several syscalls, and settings_load runs on every intercepted request.
static int cached_dir_path(char *out, size_t cap) {
  static char cached[PATH_BUF];
  static plugin_mutex_t mutex = PLUGIN_MUTEX_INITIALIZER;
  if (cached[0]) {
    if (strlen(cached) >= cap) return 0;
    snprintf(out, cap, "%s", cached);
    return 1;
  }
  plugin_mutex_lock(&mutex);
  if (!cached[0]) {
    char dirs[8][PATH_BUF];
    int count = candidate_dirs(dirs, 8);
    // An existing file takes precedence: moving it on upgrade would look
    // exactly like the settings loss this function exists to prevent.
    for (int pass = 0; pass < 2 && !cached[0]; pass++) {
      for (int i = 0; i < count && !cached[0]; i++) {
        if (!usable_dir(dirs[i])) continue;
        char path[PATH_BUF];
        if (!config_in(dirs[i], path, sizeof(path))) continue;
        int accept = pass == 0 ? file_exists(path) : plugin_dir_writable(dirs[i]);
        if (accept) snprintf(cached, sizeof(cached), "%s", path);
      }
    }
    if (!cached[0]) fallback_path(cached, sizeof(cached));
  }
  plugin_mutex_unlock(&mutex);
  if (strlen(cached) >= cap) return 0;
  snprintf(out, cap, "%s", cached);
  return 1;
}

int plugin_config_path(char *out, size_t cap) {
  // Both overrides are re-read on every call: an operator (or a test) may set
  // them after the plugin already resolved a path once.
  const char *override = plugin_getenv("CODEX_TZ_CONFIG_FILE");
  if (override && *override) {
    if (strlen(override) >= cap) return 0;
    snprintf(out, cap, "%s", override);
    return 1;
  }
  const char *dir = plugin_getenv("CODEX_TZ_CONFIG_DIR");
  if (dir && *dir && plugin_dir_exists(dir) && plugin_dir_writable(dir))
    return config_in(dir, out, cap);
  return cached_dir_path(out, cap);
}
