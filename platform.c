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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

// The host process is a Go binary linked against the UCRT, while the plugin may
// use a different CRT; read the real process environment instead of the copy.
static const char *windows_env(const char *name) {
  static const char *names[] = {
    "CODEX_TZ_CONFIG_FILE", "CODEX_TZ_EVENTS_FILE"
  };
  static char values[sizeof(names) / sizeof(names[0])][1024];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(name, names[i]) != 0) continue;
    DWORD length = GetEnvironmentVariableA(name, values[i], sizeof(values[i]));
    return length > 0 && length < sizeof(values[i]) ? values[i] : NULL;
  }
  return NULL;
}

const char *plugin_getenv(const char *name) {
  return windows_env(name);
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
// The plugin has no API to learn where the host stores its data, so the only
// reliable anchor is the library file itself: CPA discovers plugins from a
// directory that operators mount as a volume. Writing next to the binary keeps
// the file across container recreation without any extra configuration.

// Strips the trailing path component, leaving the containing directory.
static int dir_of(const char *path, char *out, size_t cap) {
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

#ifdef _WIN32

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
  return dir_of(path, out, cap);
}

#else

// Linux and macOS both resolve the library path through dladdr.
int plugin_library_dir(char *out, size_t cap) {
  Dl_info info;
  if (!dladdr((const void *)&plugin_library_dir, &info) || !info.dli_fname) return 0;
  return dir_of(info.dli_fname, out, cap);
}

#endif

int plugin_config_path(char *out, size_t cap) {
  const char *override = plugin_getenv("CODEX_TZ_CONFIG_FILE");
  if (override && *override) {
    if (strlen(override) >= cap) return 0;
    snprintf(out, cap, "%s", override);
    return 1;
  }
  char dir[1024];
  if (!plugin_library_dir(dir, sizeof(dir))) {
    if (strlen(PLUGIN_CONFIG_FALLBACK) >= cap) return 0;
    snprintf(out, cap, "%s", PLUGIN_CONFIG_FALLBACK);
    return 1;
  }
  // One separator plus the file name must still fit.
  if (strlen(dir) + strlen(PLUGIN_CONFIG_NAME) + 2 > cap) return 0;
  snprintf(out, cap, "%s/%s", dir, PLUGIN_CONFIG_NAME);
  return 1;
}
