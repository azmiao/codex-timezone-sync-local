// Codex request timezone correction.
#ifndef CODEX_TZ_REWRITE_H
#define CODEX_TZ_REWRITE_H

#include <stddef.h>

#include "settings.h"

#define REWRITE_MAX_REQUEST (12u * 1024u * 1024u)
#define REWRITE_MAX_TOKENS 150000

typedef enum {
  REWRITE_NONE = 0,   // request needed no change (already correct or no target)
  REWRITE_APPLIED = 1,
  REWRITE_FAILED = 2  // no output; caller leaves the request untouched
} rewrite_status;

// Context that is spliced into the request, derived from the target zone.
typedef struct {
  char zone[TZ_NAME_MAX];
  char date[16];
  char time[12];
  char abbr[32];
  char offset[8];
} rewrite_context;

// Build the context for `zone` at `epoch`. 0 when the zone is unusable.
int rewrite_context_build(const char *zone, long long epoch, rewrite_context *out);

// Rewrite a decoded Codex request body.
// On REWRITE_APPLIED the caller owns *out. On any other status *out is NULL.
rewrite_status rewrite_payload(const char *payload, size_t length, const tz_settings *cfg,
                               long long epoch, char **out, size_t *out_length,
                               rewrite_context *used);

// Find the first <timezone>…</timezone> value in a payload, for the activity log.
// Writes an empty string when the request carries no environment block.
void rewrite_origin_zone(const char *payload, size_t length, char *out, size_t cap);

#endif
