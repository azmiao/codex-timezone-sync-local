// Bounded in-memory activity ring. Holds no request content by design.
#include "events.h"
#include "json.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static event_record ring[EVENT_ROWS];
static int ring_head = 0;
static int ring_used = 0;
static plugin_mutex_t ring_mutex = PLUGIN_MUTEX_INITIALIZER;

void events_init(void) {
  plugin_mutex_lock(&ring_mutex);
  memset(ring, 0, sizeof(ring));
  ring_head = 0;
  ring_used = 0;
  plugin_mutex_unlock(&ring_mutex);
}

void events_add(const char *result, const char *zone_from, const char *zone_to, const char *date) {
  plugin_mutex_lock(&ring_mutex);
  event_record *record = &ring[ring_head];
  memset(record, 0, sizeof(*record));
  record->timestamp = plugin_now();
  snprintf(record->result, sizeof(record->result), "%s", result ? result : "unknown");
  snprintf(record->zone_from, sizeof(record->zone_from), "%s", zone_from ? zone_from : "");
  snprintf(record->zone_to, sizeof(record->zone_to), "%s", zone_to ? zone_to : "");
  snprintf(record->date, sizeof(record->date), "%s", date ? date : "");
  ring_head = (ring_head + 1) % EVENT_ROWS;
  if (ring_used < EVENT_ROWS) ring_used++;
  plugin_mutex_unlock(&ring_mutex);
}

void events_clear(void) {
  events_init();
}

void events_json(char *out, size_t cap) {
  if (!out || cap == 0) return;
  size_t used = 0;
  out[0] = 0;
  plugin_mutex_lock(&ring_mutex);
  for (int i = 0; i < ring_used; i++) {
    const event_record *record = &ring[(ring_head + EVENT_ROWS - 1 - i) % EVENT_ROWS];
    char *result = json_encode(record->result);
    char *from = json_encode(record->zone_from);
    char *to = json_encode(record->zone_to);
    char *date = json_encode(record->date);
    if (!result || !from || !to || !date) {
      free(result);
      free(from);
      free(to);
      free(date);
      break;
    }
    int written = snprintf(out + used, cap - used,
                           "%s{\"timestamp\":%lld,\"result\":%s,\"timezone_from\":%s,"
                           "\"timezone_to\":%s,\"date\":%s}",
                           i ? "," : "", record->timestamp, result, from, to, date);
    free(result);
    free(from);
    free(to);
    free(date);
    if (written < 0 || (size_t)written >= cap - used) break;
    used += (size_t)written;
  }
  plugin_mutex_unlock(&ring_mutex);
  out[used] = 0;
}
