// Bounded in-memory activity ring.
// Deliberately not persisted: no request text ever reaches the disk, and a CPA
// restart simply starts with an empty list.
#ifndef CODEX_TZ_EVENTS_H
#define CODEX_TZ_EVENTS_H

#include <stddef.h>

#define EVENT_ROWS 20

typedef struct {
  long long timestamp;
  char result[16];   // "rewritten", "unchanged", "failed"
  char zone_from[64];
  char zone_to[64];
  char date[16];
} event_record;

void events_init(void);
// `zone_from` may be empty when the request had no environment block.
void events_add(const char *result, const char *zone_from, const char *zone_to, const char *date);
void events_clear(void);
// Serialize the newest-first ring as a JSON array body (no surrounding braces).
void events_json(char *out, size_t cap);

#endif
