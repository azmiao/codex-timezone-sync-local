// Settings storage: one small key=value file, written atomically with 0600.
// Read on every request so a panel save takes effect immediately.
#include "settings.h"
#include "platform.h"
#include "timezone.h"

#include <stdio.h>
#include <string.h>

#define CONFIG_LINE 160
#define CONFIG_MAX 512
#define CONFIG_PATH_MAX 1200

// plugin_config_path caches the resolved directory itself, so this stays cheap
// even though settings_load runs on every intercepted request. The result is
// not cached here: the environment overrides must be re-read on each call, so
// an operator change still reaches the right file. The caller supplies the
// buffer because requests are handled concurrently.
static void config_path(char *out, size_t cap) {
  if (!plugin_config_path(out, cap))
    snprintf(out, cap, "%s", PLUGIN_CONFIG_FALLBACK);
}

int settings_zone_valid(const char *zone) {
  return tz_find(zone) != NULL;
}

int settings_copy_zone(char *out, size_t cap, const char *value) {
  if (!out || !value || !*value || strlen(value) >= cap) return 0;
  if (!settings_zone_valid(value)) return 0;
  snprintf(out, cap, "%s", value);
  return 1;
}

int settings_config_path(char *out, size_t cap) {
  if (!out || cap < 2) return 0;
  config_path(out, cap);
  return 1;
}

// Reported by the panel so a read-only plugins directory is visible instead of
// only showing up as lost settings after the next restart.
int settings_config_writable(void) {
  char path[CONFIG_PATH_MAX];
  config_path(path, sizeof(path));
  char dir[CONFIG_PATH_MAX];
  if (!plugin_dir_of(path, dir, sizeof(dir))) return 0;
  return plugin_dir_writable(dir);
}

// Match "key=value" and hand back the value; NULL when the key differs.
static const char *value_of(const char *line, const char *key) {
  size_t length = strlen(key);
  if (strncmp(line, key, length) != 0 || line[length] != '=') return NULL;
  return line + length + 1;
}

// Earlier builds wrote next to the loaded library. Because the host loads
// plugins from a scratch copy, that path is /tmp/cliproxy-pluginhost/pid-N/.
// On the first run after the upgrade, adopt such a file so a user's settings
// survive the switch instead of silently reverting to defaults.
static void adopt_legacy_file(const char *target) {
  // Runs at most once per process: settings_load is on the request hot path.
  static int attempted = 0;
  static plugin_mutex_t mutex = PLUGIN_MUTEX_INITIALIZER;
  plugin_mutex_lock(&mutex);
  int first = !attempted;
  attempted = 1;
  plugin_mutex_unlock(&mutex);
  if (!first) return;
  char legacy[CONFIG_PATH_MAX];
  if (!plugin_library_dir(legacy, sizeof(legacy))) return;
  size_t dir_length = strlen(legacy);
  size_t name_length = strlen(PLUGIN_CONFIG_NAME);
  if (dir_length + 1 + name_length + 1 > CONFIG_PATH_MAX) return;
  char legacy_path[CONFIG_PATH_MAX];
  memcpy(legacy_path, legacy, dir_length);
  legacy_path[dir_length] = '/';
  memcpy(legacy_path + dir_length + 1, PLUGIN_CONFIG_NAME, name_length + 1);
  if (strcmp(legacy_path, target) == 0) return;
  FILE *source = fopen(legacy_path, "r");
  if (!source) return;
  char data[CONFIG_MAX];
  size_t length = fread(data, 1, sizeof(data), source);
  int readable = !ferror(source);
  fclose(source);
  if (!readable || length == 0) return;
  if (plugin_write_file_atomic(target, data, length)) remove(legacy_path);
}

void settings_load(tz_settings *out) {
  memset(out, 0, sizeof(*out));
  out->enabled = 1;
  out->logging = 1;
  snprintf(out->local_zone, sizeof(out->local_zone), "%s", TZ_DEFAULT_LOCAL);
  snprintf(out->target_zone, sizeof(out->target_zone), "%s", TZ_DEFAULT_TARGET);
  char path[CONFIG_PATH_MAX];
  config_path(path, sizeof(path));
  FILE *file = fopen(path, "r");
  if (!file) {
    // No file at the new location: migrate one left by an older build.
    adopt_legacy_file(path);
    file = fopen(path, "r");
    if (!file) return;
  }
  char line[CONFIG_LINE];
  while (fgets(line, sizeof(line), file)) {
    line[strcspn(line, "\r\n")] = 0;
    const char *value = NULL;
    // A zone name that is unknown or too long is stale configuration: keep the
    // default rather than starting up with an unusable zone.
    if ((value = value_of(line, "enabled")) != NULL && value[0] && !value[1])
      out->enabled = value[0] == '1';
    else if ((value = value_of(line, "logging")) != NULL && value[0] && !value[1])
      out->logging = value[0] == '1';
    else if ((value = value_of(line, "local_zone")) != NULL)
      settings_copy_zone(out->local_zone, sizeof(out->local_zone), value);
    else if ((value = value_of(line, "target_zone")) != NULL)
      settings_copy_zone(out->target_zone, sizeof(out->target_zone), value);
  }
  fclose(file);
}

int settings_save(const tz_settings *cfg) {
  if (!cfg || !settings_zone_valid(cfg->local_zone) || !settings_zone_valid(cfg->target_zone))
    return 0;
  char path[CONFIG_PATH_MAX];
  config_path(path, sizeof(path));
  if (strlen(path) > 1024) return 0;
  char data[CONFIG_MAX];
  int length = snprintf(data, sizeof(data),
                        "enabled=%d\nlogging=%d\nlocal_zone=%s\ntarget_zone=%s\n",
                        cfg->enabled ? 1 : 0, cfg->logging ? 1 : 0,
                        cfg->local_zone, cfg->target_zone);
  if (length <= 0 || length >= (int)sizeof(data)) return 0;
  return plugin_write_file_atomic(path, data, (size_t)length);
}
