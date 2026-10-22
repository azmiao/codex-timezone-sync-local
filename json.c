// Minimal JSON scanner and text splicing helpers.
#include "json.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int json_parse(const char *text, size_t length, json_token *tokens, int capacity) {
  int count = 0, parent = -1;
  for (size_t i = 0; i < length;) {
    unsigned char c = (unsigned char)text[i];
    if (isspace(c) || c == ':' || c == ',') { i++; continue; }
    if (c == '{' || c == '[') {
      if (count >= capacity) return -1;
      int id = count++;
      tokens[id] = (json_token){(int)i, -1, parent, c == '{' ? JSON_OBJECT : JSON_ARRAY};
      parent = id;
      i++;
      continue;
    }
    if (c == '}' || c == ']') {
      if (parent < 0 || tokens[parent].type != (c == '}' ? JSON_OBJECT : JSON_ARRAY)) return -1;
      tokens[parent].end = (int)i + 1;
      parent = tokens[parent].parent;
      i++;
      continue;
    }
    if (c == '"') {
      size_t start = ++i;
      for (; i < length; i++) {
        if (text[i] == '\\') {
          if (++i >= length) return -1;
          continue;
        }
        if (text[i] == '"') break;
        if ((unsigned char)text[i] < 32) return -1;
      }
      if (i >= length || count >= capacity) return -1;
      tokens[count++] = (json_token){(int)start, (int)i, parent, JSON_STRING};
      i++;
      continue;
    }
    if (c == '-' || isdigit(c) || c == 't' || c == 'f' || c == 'n') {
      size_t start = i;
      while (i < length && text[i] != ',' && text[i] != ']' && text[i] != '}' &&
             !isspace((unsigned char)text[i])) i++;
      if (start == i || count >= capacity) return -1;
      tokens[count++] = (json_token){(int)start, (int)i, parent, JSON_PRIMITIVE};
      continue;
    }
    return -1;
  }
  return parent < 0 && count > 0 ? count : -1;
}

int json_next(const json_token *tokens, int count, int index) {
  int end = tokens[index].end;
  index++;
  while (index < count && tokens[index].start < end) index++;
  return index;
}

int json_field(const char *text, const json_token *tokens, int count, int object, const char *name) {
  if (object < 0 || tokens[object].type != JSON_OBJECT) return -1;
  size_t length = strlen(name);
  for (int id = object + 1; id < count && tokens[id].start < tokens[object].end;) {
    if (tokens[id].parent != object || tokens[id].type != JSON_STRING) return -1;
    int value = id + 1;
    if (value >= count || tokens[value].parent != object) return -1;
    if ((size_t)(tokens[id].end - tokens[id].start) == length &&
        memcmp(text + tokens[id].start, name, length) == 0) return value;
    id = json_next(tokens, count, value);
  }
  return -1;
}

int json_field_any(const char *text, const json_token *tokens, int count, int object,
                   const char *const *names) {
  for (int i = 0; names[i]; i++) {
    int found = json_field(text, tokens, count, object, names[i]);
    if (found >= 0) return found;
  }
  return -1;
}

int json_string_is(const char *text, json_token token, const char *expected) {
  size_t length = strlen(expected);
  return token.type == JSON_STRING && (size_t)(token.end - token.start) == length &&
         memcmp(text + token.start, expected, length) == 0;
}

int json_contains(const char *text, size_t length, const char *needle) {
  size_t size = strlen(needle);
  if (size == 0 || size > length) return 0;
  for (size_t i = 0; i + size <= length; i++)
    if (memcmp(text + i, needle, size) == 0) return 1;
  return 0;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int read_hex4(const char *text, uint32_t *value) {
  *value = 0;
  for (int i = 0; i < 4; i++) {
    int digit = hex_value(text[i]);
    if (digit < 0) return 0;
    *value = (*value << 4) | (uint32_t)digit;
  }
  return 1;
}

char *json_decode(const char *text, json_token token) {
  size_t length = (size_t)(token.end - token.start);
  char *buffer = (char *)malloc(length + 1);
  if (!buffer) return NULL;
  size_t written = 0;
  const char *source = text + token.start;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)source[i];
    if (c != '\\') {
      if (c == 0) { free(buffer); return NULL; }
      buffer[written++] = (char)c;
      continue;
    }
    if (++i >= length) { free(buffer); return NULL; }
    char escape = source[i];
    const char *simple = "\"\\/bfnrt";
    const char *mapping = "\"\\/\b\f\n\r\t";
    const char *hit = strchr(simple, escape);
    if (hit) {
      buffer[written++] = mapping[hit - simple];
      continue;
    }
    if (escape != 'u' || i + 4 >= length) { free(buffer); return NULL; }
    uint32_t code = 0;
    if (!read_hex4(source + i + 1, &code)) { free(buffer); return NULL; }
    i += 4;
    if (code >= 0xD800 && code <= 0xDBFF) {
      uint32_t low = 0;
      if (i + 6 >= length || source[i + 1] != '\\' || source[i + 2] != 'u' ||
          !read_hex4(source + i + 3, &low) || low < 0xDC00 || low > 0xDFFF) {
        free(buffer);
        return NULL;
      }
      code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
      i += 6;
    } else if (code >= 0xDC00 && code <= 0xDFFF) {
      free(buffer);
      return NULL;
    }
    if (code == 0) { free(buffer); return NULL; }
    if (code < 0x80) {
      buffer[written++] = (char)code;
    } else if (code < 0x800) {
      buffer[written++] = (char)(0xC0 | (code >> 6));
      buffer[written++] = (char)(0x80 | (code & 63));
    } else if (code < 0x10000) {
      buffer[written++] = (char)(0xE0 | (code >> 12));
      buffer[written++] = (char)(0x80 | ((code >> 6) & 63));
      buffer[written++] = (char)(0x80 | (code & 63));
    } else {
      buffer[written++] = (char)(0xF0 | (code >> 18));
      buffer[written++] = (char)(0x80 | ((code >> 12) & 63));
      buffer[written++] = (char)(0x80 | ((code >> 6) & 63));
      buffer[written++] = (char)(0x80 | (code & 63));
    }
  }
  buffer[written] = 0;
  return buffer;
}

char *json_encode(const char *text) {
  size_t length = strlen(text);
  if (length > (SIZE_MAX - 3) / 6) return NULL;
  char *out = (char *)malloc(length * 6 + 3);
  if (!out) return NULL;
  static const char hex[] = "0123456789abcdef";
  size_t written = 0;
  out[written++] = '"';
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\') {
      out[written++] = '\\';
      out[written++] = (char)c;
    } else if (c == '\n') {
      out[written++] = '\\';
      out[written++] = 'n';
    } else if (c == '\r') {
      out[written++] = '\\';
      out[written++] = 'r';
    } else if (c == '\t') {
      out[written++] = '\\';
      out[written++] = 't';
    } else if (c < 32) {
      out[written++] = '\\';
      out[written++] = 'u';
      out[written++] = '0';
      out[written++] = '0';
      out[written++] = hex[c >> 4];
      out[written++] = hex[c & 15];
    } else {
      out[written++] = (char)c;
    }
  }
  out[written++] = '"';
  out[written] = 0;
  return out;
}

char *json_splice(const char *source, size_t length, size_t at, size_t remove,
                  const char *insert, size_t insert_length, size_t *out_length) {
  if (at > length || remove > length - at) return NULL;
  size_t size = length - remove + insert_length;
  char *out = (char *)malloc(size + 1);
  if (!out) return NULL;
  memcpy(out, source, at);
  memcpy(out + at, insert, insert_length);
  memcpy(out + at + insert_length, source + at + remove, length - at - remove);
  out[size] = 0;
  if (out_length) *out_length = size;
  return out;
}
