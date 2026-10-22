// Offline IANA timezone engine.
// Rules come from the generated timezone_table.h; no host zoneinfo, no network.
#ifndef CODEX_TZ_TIMEZONE_H
#define CODEX_TZ_TIMEZONE_H

#include <stddef.h>

#define TZ_NAME_MAX 64

// One explicit state change: from `epoch` (UTC seconds) the zone shows
// `offset` and `abbr` until the next row.
typedef struct {
  long long epoch;
  int offset;
  const char *abbr;
} tz_row;

typedef struct {
  const char *name;
  const char *rule;   // POSIX TZ string, or NULL for list zones
  const tz_row *rows; // explicit transitions, or NULL for rule zones
  int row_count;
} tz_entry;

typedef struct {
  int utc_offset;   // seconds east of UTC
  char abbr[32];
  char date[16];    // YYYY-MM-DD in the zone
  char time[12];    // HH:MM:SS in the zone
} tz_now;

int tz_count(void);
const tz_entry *tz_at(int index);
const tz_entry *tz_find(const char *name);
// 1 on success, 0 when the name is unknown or its rule cannot be parsed.
int tz_evaluate(const char *name, long long epoch, tz_now *out);
void tz_format_offset(int seconds, char *out, size_t cap);

#endif
