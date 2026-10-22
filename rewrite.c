// Codex request timezone correction.
//
// Two behaviours, both driven by the panel's local/target zone pair:
//   * an existing <environment_context> block is normalized in place, so the
//     local zone and its date are replaced by the target zone and its date;
//   * a request without such a block gets one appended, so the model still
//     sees a consistent date/timezone.
// Every byte outside the edited text is preserved: the payload is never
// re-serialized.
#include "rewrite.h"
#include "json.h"
#include "timezone.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strncasecmp _strnicmp
#endif

#define ENV_FIELDS_LIMIT 256
#define CONTEXT_MAX 320

int rewrite_context_build(const char *zone, long long epoch, rewrite_context *out) {
  tz_now now;
  if (!out || !tz_evaluate(zone, epoch, &now)) return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->zone, sizeof(out->zone), "%s", zone);
  snprintf(out->date, sizeof(out->date), "%s", now.date);
  snprintf(out->time, sizeof(out->time), "%s", now.time);
  snprintf(out->abbr, sizeof(out->abbr), "%s", now.abbr);
  tz_format_offset(now.utc_offset, out->offset, sizeof(out->offset));
  return 1;
}

// The injected context is itself an <environment_context> block, so a later
// pass normalizes it in place instead of appending a second copy. That also
// keeps the date correct when the request is retried across midnight.
static void context_text(const rewrite_context *context, char *out, size_t cap) {
  snprintf(out, cap,
           "<environment_context>\n<timezone>%s</timezone>\n<current_date>%s</current_date>\n"
           "</environment_context>",
           context->zone, context->date);
}

static const char *find_ci(const char *haystack, const char *needle) {
  size_t length = strlen(needle);
  for (const char *cursor = haystack; *cursor; cursor++)
    if (strncasecmp(cursor, needle, length) == 0) return cursor;
  return NULL;
}

// Replace `remove` bytes at `at` with `insert`, taking ownership of *text.
static char *replace_range(char *text, size_t at, size_t remove, const char *insert) {
  size_t length = strlen(text);
  size_t next_length = 0;
  char *next = json_splice(text, length, at, remove, insert, strlen(insert), &next_length);
  free(text);
  (void)next_length;
  return next;
}

// Normalize one <timezone>…</timezone> (or <current_date>…) pair inside a block.
static char *patch_env_field(char *block, const char *field, const char *value) {
  char open_tag[48], close_tag[52];
  snprintf(open_tag, sizeof(open_tag), "<%s>", field);
  snprintf(close_tag, sizeof(close_tag), "</%s>", field);
  size_t cursor = 0;
  int found = 0;
  while (block) {
    const char *open = find_ci(block + cursor, open_tag);
    if (!open) break;
    const char *close = find_ci(open + strlen(open_tag), close_tag);
    if (!close) break;
    found = 1;
    size_t at = (size_t)(open - block) + strlen(open_tag);
    size_t length = (size_t)(close - (block + at));
    if (length != strlen(value) || memcmp(block + at, value, length) != 0) {
      block = replace_range(block, at, length, value);
      if (!block) return NULL;
    }
    cursor = at + strlen(value) + strlen(close_tag);
  }
  if (!block || found) return block;
  // The field is absent: add it as a self-closing tag replacement or before the
  // closing tag, keeping the block well formed either way.
  char self_tag[48], insertion[160];
  snprintf(self_tag, sizeof(self_tag), "<%s/>", field);
  const char *self = find_ci(block, self_tag);
  if (self) {
    snprintf(insertion, sizeof(insertion), "<%s>%s</%s>", field, value, field);
    return replace_range(block, (size_t)(self - block), strlen(self_tag), insertion);
  }
  const char *closing = find_ci(block, "</environment_context");
  if (!closing) return block;
  snprintf(insertion, sizeof(insertion), "<%s>%s</%s>\n", field, value, field);
  return replace_range(block, (size_t)(closing - block), 0, insertion);
}

// Normalize every environment block in one decoded text. *found is set when at
// least one block was seen; the returned buffer is always owned by the caller.
static char *patch_environment_text(char *text, const rewrite_context *context, int *found) {
  size_t cursor = 0;
  *found = 0;
  while (text) {
    const char *opening = find_ci(text + cursor, "<environment_context");
    if (!opening) break;
    const char *open_end = strchr(opening, '>');
    if (!open_end) break;
    const char *closing = find_ci(open_end + 1, "</environment_context");
    if (!closing) break;
    const char *close_end = strchr(closing, '>');
    if (!close_end) break;
    *found = 1;
    size_t offset = (size_t)(opening - text);
    size_t count = (size_t)(close_end + 1 - opening);
    char *block = (char *)malloc(count + 1);
    if (!block) { free(text); return NULL; }
    memcpy(block, opening, count);
    block[count] = 0;
    block = patch_env_field(block, "timezone", context->zone);
    if (block) block = patch_env_field(block, "current_date", context->date);
    if (!block) { free(text); return NULL; }
    if (strlen(block) != count || memcmp(opening, block, count) != 0) {
      size_t block_length = strlen(block);
      char *next = json_splice(text, strlen(text), offset, count, block, block_length, NULL);
      free(block);
      if (!next) { free(text); return NULL; }
      text = next;
      cursor = offset + block_length;
    } else {
      cursor = offset + count;
      free(block);
    }
  }
  return text;
}

static void push_string_token(const json_token *tokens, int index, int *positions, int *count) {
  if (index < 0 || tokens[index].type != JSON_STRING || *count >= ENV_FIELDS_LIMIT) return;
  for (int i = 0; i < *count; i++)
    if (positions[i] == index) return;
  positions[(*count)++] = index;
}

static void collect_contents(const char *text, const json_token *tokens, int count, int value,
                             int *positions, int *found) {
  if (value < 0) return;
  if (tokens[value].type == JSON_STRING) {
    push_string_token(tokens, value, positions, found);
    return;
  }
  if (tokens[value].type != JSON_ARRAY) return;
  for (int id = value + 1; id < count && tokens[id].start < tokens[value].end;
       id = json_next(tokens, count, id)) {
    if (tokens[id].type != JSON_OBJECT) continue;
    int type = json_field(text, tokens, count, id, "type");
    if (type < 0) continue;
    if (!json_string_is(text, tokens[type], "text") &&
        !json_string_is(text, tokens[type], "input_text")) continue;
    push_string_token(tokens, json_field(text, tokens, count, id, "text"), positions, found);
  }
}

static void collect_messages(const char *text, const json_token *tokens, int count, int list,
                             int *positions, int *found) {
  if (list < 0 || tokens[list].type != JSON_ARRAY) return;
  for (int id = list + 1; id < count && tokens[id].start < tokens[list].end;
       id = json_next(tokens, count, id)) {
    if (tokens[id].type != JSON_OBJECT) continue;
    int role = json_field(text, tokens, count, id, "role");
    if (role < 0) continue;
    if (!json_string_is(text, tokens[role], "system") &&
        !json_string_is(text, tokens[role], "developer") &&
        !json_string_is(text, tokens[role], "user")) continue;
    collect_contents(text, tokens, count, json_field(text, tokens, count, id, "content"),
                     positions, found);
  }
}

// Build "<prefix><encoded context><suffix>" without a fixed-size stack buffer:
// the encoded context can be several times longer than the raw text.
static char *wrap_encoded(const char *prefix, const char *encoded, const char *suffix) {
  size_t prefix_length = strlen(prefix);
  size_t encoded_length = strlen(encoded);
  size_t suffix_length = strlen(suffix);
  char *out = (char *)malloc(prefix_length + encoded_length + suffix_length + 1);
  if (!out) return NULL;
  memcpy(out, prefix, prefix_length);
  memcpy(out + prefix_length, encoded, encoded_length);
  memcpy(out + prefix_length + encoded_length, suffix, suffix_length);
  out[prefix_length + encoded_length + suffix_length] = 0;
  return out;
}

// Insert the context where the Codex request expects instructions.
static char *insert_context(const char *payload, size_t length, const json_token *tokens,
                            int instructions, int input, int messages,
                            const char *context, size_t *out_length) {
  char *encoded = json_encode(context);
  if (!encoded) return NULL;
  char *out = NULL;
  if (instructions >= 0 && tokens[instructions].type == JSON_STRING) {
    // Append inside the existing string literal: the encoded quotes must be
    // dropped, otherwise they would terminate the string early.
    size_t inner = strlen(encoded) - 2;
    char *text = (char *)malloc(inner + 1);
    if (text) {
      memcpy(text, encoded + 1, inner);
      text[inner] = 0;
      char *suffix = wrap_encoded("\\n\\n", text, "");
      free(text);
      if (suffix) {
        out = json_splice(payload, length, (size_t)tokens[instructions].end, 0,
                          suffix, strlen(suffix), out_length);
        free(suffix);
      }
    }
  } else if (instructions >= 0 && tokens[instructions].type == JSON_PRIMITIVE &&
             (size_t)(tokens[instructions].end - tokens[instructions].start) == 4 &&
             memcmp(payload + tokens[instructions].start, "null", 4) == 0) {
    out = json_splice(payload, length, (size_t)tokens[instructions].start, 4,
                      encoded, strlen(encoded), out_length);
  } else if (instructions >= 0) {
    // Unexpected type: refuse rather than risk a duplicate instruction block.
    out = NULL;
  } else if (input >= 0 && tokens[input].type == JSON_ARRAY) {
    char *field = wrap_encoded("\"instructions\":", encoded, ",");
    if (field) {
      out = json_splice(payload, length, (size_t)tokens[0].start + 1, 0,
                        field, strlen(field), out_length);
      free(field);
    }
  } else if (messages >= 0 && tokens[messages].type == JSON_ARRAY) {
    // The array may be empty, in which case no trailing comma is needed.
    size_t at = (size_t)tokens[messages].start + 1;
    size_t check = at;
    while (check < length && isspace((unsigned char)payload[check])) check++;
    int needs_comma = check < length && payload[check] != ']';
    char *message = wrap_encoded("{\"role\":\"developer\",\"content\":", encoded,
                                 needs_comma ? "}," : "}");
    if (message) {
      out = json_splice(payload, length, at, 0, message, strlen(message), out_length);
      free(message);
    }
  }
  free(encoded);
  return out;
}

static int safe_zone(const char *value) {
  size_t length = strlen(value);
  if (length == 0 || length >= TZ_NAME_MAX) return 0;
  for (size_t i = 0; i < length; i++)
    if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_+-", value[i]))
      return 0;
  return 1;
}

void rewrite_origin_zone(const char *payload, size_t length, char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (!payload || length == 0 || length > REWRITE_MAX_REQUEST) return;
  json_token *tokens = (json_token *)malloc(8192 * sizeof(json_token));
  if (!tokens) return;
  int count = json_parse(payload, length, tokens, 8192);
  for (int i = 0; i < count; i++) {
    if (tokens[i].type != JSON_STRING) continue;
    char *text = json_decode(payload, tokens[i]);
    if (!text) continue;
    const char *env = find_ci(text, "<environment_context");
    const char *tag = env ? find_ci(env, "<timezone>") : NULL;
    const char *close = tag ? find_ci(tag, "</timezone>") : NULL;
    if (close && (size_t)(close - tag) < 96) {
      char value[96];
      const char *value_start = tag + strlen("<timezone>");
      size_t value_length = (size_t)(close - value_start);
      if (value_length < sizeof(value)) {
        memcpy(value, value_start, value_length);
        value[value_length] = 0;
        if (safe_zone(value)) {
          snprintf(out, cap, "%s", value);
          free(text);
          break;
        }
      }
    }
    free(text);
  }
  free(tokens);
}

rewrite_status rewrite_payload(const char *payload, size_t length, const tz_settings *cfg,
                               long long epoch, char **out, size_t *out_length,
                               rewrite_context *used) {
  if (out) *out = NULL;
  if (!payload || !cfg || length == 0 || length > REWRITE_MAX_REQUEST) return REWRITE_FAILED;
  rewrite_context context;
  if (!rewrite_context_build(cfg->target_zone, epoch, &context)) return REWRITE_FAILED;
  if (used) *used = context;

  json_token *tokens = (json_token *)malloc(REWRITE_MAX_TOKENS * sizeof(json_token));
  if (!tokens) return REWRITE_FAILED;
  int count = json_parse(payload, length, tokens, REWRITE_MAX_TOKENS);
  if (count < 1 || tokens[0].type != JSON_OBJECT) {
    free(tokens);
    return REWRITE_FAILED;
  }

  int instructions = json_field(payload, tokens, count, 0, "instructions");
  int input = json_field(payload, tokens, count, 0, "input");
  int messages = json_field(payload, tokens, count, 0, "messages");

  int positions[ENV_FIELDS_LIMIT];
  int used_positions = 0;
  collect_contents(payload, tokens, count, instructions, positions, &used_positions);
  int system = json_field(payload, tokens, count, 0, "system");
  collect_contents(payload, tokens, count, system, positions, &used_positions);
  if (input >= 0 && tokens[input].type == JSON_STRING)
    push_string_token(tokens, input, positions, &used_positions);
  else
    collect_messages(payload, tokens, count, input, positions, &used_positions);
  collect_messages(payload, tokens, count, messages, positions, &used_positions);

  // Edit from the end of the document backwards so earlier offsets stay valid.
  for (int a = 0; a < used_positions; a++)
    for (int b = a + 1; b < used_positions; b++)
      if (tokens[positions[a]].start < tokens[positions[b]].start) {
        int swap = positions[a];
        positions[a] = positions[b];
        positions[b] = swap;
      }

  char *current = (char *)malloc(length + 1);
  if (!current) { free(tokens); return REWRITE_FAILED; }
  memcpy(current, payload, length);
  current[length] = 0;
  size_t current_length = length;
  int saw_environment = 0, changed = 0;

  for (int i = 0; i < used_positions; i++) {
    json_token token = tokens[positions[i]];
    char *decoded = json_decode(payload, token);
    if (!decoded) { free(current); free(tokens); return REWRITE_FAILED; }
    int found = 0;
    char *normalized = patch_environment_text(decoded, &context, &found);
    if (!normalized) { free(current); free(tokens); return REWRITE_FAILED; }
    if (!found) { free(normalized); continue; }
    saw_environment = 1;
    char *encoded = json_encode(normalized);
    free(normalized);
    if (!encoded) { free(current); free(tokens); return REWRITE_FAILED; }
    size_t at = (size_t)token.start - 1;
    size_t old_length = (size_t)(token.end - token.start) + 2;
    if (old_length != strlen(encoded) || memcmp(current + at, encoded, old_length) != 0) {
      size_t next_length = 0;
      char *next = json_splice(current, current_length, at, old_length, encoded, strlen(encoded), &next_length);
      free(encoded);
      if (!next) { free(current); free(tokens); return REWRITE_FAILED; }
      free(current);
      current = next;
      current_length = next_length;
      changed = 1;
    } else {
      free(encoded);
    }
  }

  if (saw_environment) {
    free(tokens);
    if (!changed) { free(current); return REWRITE_NONE; }
    *out = current;
    if (out_length) *out_length = current_length;
    return REWRITE_APPLIED;
  }
  free(current);

  char context_buffer[CONTEXT_MAX];
  context_text(&context, context_buffer, sizeof(context_buffer));
  size_t inserted_length = 0;
  char *inserted = insert_context(payload, length, tokens, instructions, input, messages,
                                  context_buffer, &inserted_length);
  free(tokens);
  if (!inserted) return REWRITE_FAILED;
  *out = inserted;
  if (out_length) *out_length = inserted_length;
  return REWRITE_APPLIED;
}
