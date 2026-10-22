// Minimal JSON scanning used by the interceptor.
// The plugin never re-serializes a request: it locates fields in the original
// bytes and splices text in, so every unrelated byte survives untouched.
#ifndef CODEX_TZ_JSON_H
#define CODEX_TZ_JSON_H

#include <stddef.h>

typedef struct {
  int start;
  int end;
  int parent;
  int type;
} json_token;

enum { JSON_OBJECT = 1, JSON_ARRAY = 2, JSON_STRING = 3, JSON_PRIMITIVE = 4 };

typedef struct {
  void *ptr;
  size_t len;
} plugin_buffer;

// Returns the token count, or -1 when the document is malformed or exceeds
// `capacity`. Callers must treat -1 as "leave the request alone".
int json_parse(const char *text, size_t length, json_token *tokens, int capacity);
int json_next(const json_token *tokens, int count, int index);
// Field lookup on an object token. Keys are compared byte-for-byte.
int json_field(const char *text, const json_token *tokens, int count, int object, const char *name);
// First matching field among a NULL-terminated list of alternative names.
int json_field_any(const char *text, const json_token *tokens, int count, int object,
                   const char *const *names);
int json_string_is(const char *text, json_token token, const char *expected);
// Decode a JSON string token into a fresh NUL-terminated UTF-8 buffer.
char *json_decode(const char *text, json_token token);
// Encode UTF-8 text as a quoted JSON string literal.
char *json_encode(const char *text);
// Copy [start,end) with `insert` spliced in, replacing `remove` bytes at `at`.
char *json_splice(const char *source, size_t length, size_t at, size_t remove,
                  const char *insert, size_t insert_length, size_t *out_length);
int json_contains(const char *text, size_t length, const char *needle);

#endif
