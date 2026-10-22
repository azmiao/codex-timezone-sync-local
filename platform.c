// Portability layer implementation. No third-party dependencies.
// _POSIX_C_SOURCE / _DARWIN_C_SOURCE must be defined before any libc header so
// that mkstemp, fchmod and friends are declared under -std=c11.
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
