// Durable plugin settings.
// Only two timezone names, three flags and a bounded activity ring are kept.
#ifndef CODEX_TZ_SETTINGS_H
#define CODEX_TZ_SETTINGS_H

#include <stddef.h>

#include "timezone.h"

#define TZ_DEFAULT_LOCAL "Asia/Shanghai"
#define TZ_DEFAULT_TARGET "America/Los_Angeles"

typedef struct {
  int enabled;
  int logging;
  char local_zone[TZ_NAME_MAX];
  char target_zone[TZ_NAME_MAX];
} tz_settings;

// Missing or unreadable configuration falls back to defaults; the plugin never
// refuses to start because of a settings problem.
void settings_load(tz_settings *out);
// 1 when written atomically, 0 on failure (the previous file is preserved).
int settings_save(const tz_settings *cfg);
// 1 when the zone name is known to the offline table.
int settings_zone_valid(const char *zone);
// 1 when copied; rejects unknown zones and names that do not fit.
int settings_copy_zone(char *out, size_t cap, const char *value);
// Absolute path of the settings file actually in use, for diagnostics.
int settings_config_path(char *out, size_t cap);
// 1 when the settings directory accepts writes, so a save will persist.
int settings_config_writable(void);

#endif
