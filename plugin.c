// codex-timezone-sync-local: CLIProxyAPI C ABI v1 request interceptor.
//
// Rewrites the date/timezone context of Codex requests using an offline IANA
// table. No network access, no host helper service, no state file: the panel
// stores a local zone and a target zone, and the interceptor derives everything
// else from them. Fail-open everywhere: a request is never blocked.
#include "events.h"
#include "json.h"
#include "platform.h"
#include "rewrite.h"
#include "settings.h"
#include "timezone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashboard_bundle.h"

#define ABI_VERSION 1
#define VERSION "0.7.3"
#define BODY_MAX (12u * 1024u * 1024u)
#define TOKENS_MAX 4096
#define ROUTE_PREFIX "/v0/management/plugins/codex-timezone-sync-local/"
#define PANEL_ROUTE "/v0/resource/plugins/codex-timezone-sync-local/panel"

typedef int (*plugin_call_fn)(const char *, const unsigned char *, size_t, plugin_buffer *);
typedef void (*plugin_free_fn)(void *, size_t);
typedef void (*plugin_shutdown_fn)(void);

typedef struct {
  unsigned int abi_version;
  plugin_call_fn call;
  plugin_free_fn free_buffer;
  plugin_shutdown_fn shutdown;
} cliproxy_plugin_api;

static void reply(plugin_buffer *out, const char *text) {
  if (!out || !text) return;
  size_t length = strlen(text);
  out->ptr = malloc(length ? length : 1);
  if (out->ptr) {
    memcpy(out->ptr, text, length);
    out->len = length;
  }
}

static const char *base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t length) {
  size_t size = ((length + 2) / 3) * 4;
  char *out = (char *)malloc(size + 1);
  if (!out) return NULL;
  size_t written = 0;
  for (size_t i = 0; i < length; i += 3) {
    size_t left = length - i;
    unsigned a = data[i];
    unsigned b = left > 1 ? data[i + 1] : 0;
    unsigned c = left > 2 ? data[i + 2] : 0;
    out[written++] = base64_chars[a >> 2];
    out[written++] = base64_chars[((a & 3) << 4) | (b >> 4)];
    out[written++] = left > 1 ? base64_chars[((b & 15) << 2) | (c >> 6)] : '=';
    out[written++] = left > 2 ? base64_chars[c & 63] : '=';
  }
  out[written] = 0;
  return out;
}

static int base64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static unsigned char *base64_decode(const char *text, size_t length, size_t *out_length) {
  if (length % 4 || length / 4 * 3 > BODY_MAX) return NULL;
  size_t padding = (length && text[length - 1] == '=') + (length > 1 && text[length - 2] == '=');
  size_t size = length / 4 * 3 - padding;
  unsigned char *out = (unsigned char *)malloc(size + 1);
  if (!out) return NULL;
  size_t written = 0;
  for (size_t i = 0; i < length; i += 4) {
    int a = base64_value((unsigned char)text[i]);
    int b = base64_value((unsigned char)text[i + 1]);
    int c = text[i + 2] == '=' ? 0 : base64_value((unsigned char)text[i + 2]);
    int d = text[i + 3] == '=' ? 0 : base64_value((unsigned char)text[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0 ||
        (i + 4 < length && (text[i + 2] == '=' || text[i + 3] == '=')) ||
        (text[i + 2] == '=' && text[i + 3] != '=')) {
      free(out);
      return NULL;
    }
    unsigned value = ((unsigned)a << 18) | ((unsigned)b << 12) | ((unsigned)c << 6) | (unsigned)d;
    if (written < size) out[written++] = (unsigned char)(value >> 16);
    if (written < size) out[written++] = (unsigned char)(value >> 8);
    if (written < size) out[written++] = (unsigned char)value;
  }
  out[size] = 0;
  *out_length = size;
  return out;
}

// Respond with a JSON body the CPA management frontend can render directly.
static void json_response(plugin_buffer *out, int status, const char *body) {
  char *encoded = base64_encode((const unsigned char *)body, strlen(body));
  if (!encoded) return;
  size_t capacity = strlen(encoded) + 220;
  char *message = (char *)malloc(capacity);
  if (message) {
    snprintf(message, capacity,
             "{\"ok\":true,\"result\":{\"StatusCode\":%d,\"Headers\":{"
             "\"Content-Type\":[\"application/json; charset=utf-8\"],"
             "\"Cache-Control\":[\"no-store\"]},\"Body\":\"%s\"}}",
             status, encoded);
    reply(out, message);
    free(message);
  }
  free(encoded);
}

static void html_response(plugin_buffer *out, const unsigned char *html, size_t length) {
  char *encoded = base64_encode(html, length);
  if (!encoded) return;
  size_t capacity = strlen(encoded) + 220;
  char *message = (char *)malloc(capacity);
  if (message) {
    snprintf(message, capacity,
             "{\"ok\":true,\"result\":{\"StatusCode\":200,\"Headers\":{"
             "\"Content-Type\":[\"text/html; charset=utf-8\"],"
             "\"Cache-Control\":[\"no-store\"],"
             "\"X-Content-Type-Options\":[\"nosniff\"]},\"Body\":\"%s\"}}",
             encoded);
    reply(out, message);
    free(message);
  }
  free(encoded);
}

// ---------------------------------------------------------------------------
// Request interception
// ---------------------------------------------------------------------------

static const char *const kToFormat[] = {"ToFormat", "to_format", NULL};
static const char *const kBody[] = {"Body", "body", NULL};

// The activity ring is opt-out: it holds no request content, but a user who
// turned logging off should still see an empty list.
static void note_event(const tz_settings *cfg, const char *result, const char *from,
                       const char *to, const char *date) {
  if (!cfg->logging) return;
  events_add(result, from, to, date);
}

static void handle_request(const unsigned char *request, size_t length, plugin_buffer *response) {
  if (!request || !length || length > BODY_MAX) return;
  const char *raw = (const char *)request;
  json_token *tokens = (json_token *)malloc(TOKENS_MAX * sizeof(json_token));
  if (!tokens) return;
  int count = json_parse(raw, length, tokens, TOKENS_MAX);
  if (count < 1 || tokens[0].type != JSON_OBJECT) {
    free(tokens);
    return;
  }
  int object = 0;
  int wrapped = json_field(raw, tokens, count, object, "request");
  if (wrapped >= 0 && tokens[wrapped].type == JSON_OBJECT) object = wrapped;
  int to_format = json_field_any(raw, tokens, count, object, kToFormat);
  if (to_format < 0 || !json_string_is(raw, tokens[to_format], "codex")) {
    free(tokens);
    return;
  }
  int body = json_field_any(raw, tokens, count, object, kBody);
  if (body < 0 || tokens[body].type != JSON_STRING) {
    free(tokens);
    return;
  }
  tz_settings cfg;
  settings_load(&cfg);
  if (!cfg.enabled) {
    free(tokens);
    return;
  }
  size_t payload_length = 0;
  unsigned char *payload = base64_decode(raw + tokens[body].start,
                                         (size_t)(tokens[body].end - tokens[body].start),
                                         &payload_length);
  free(tokens);
  if (!payload) {
    note_event(&cfg, "failed", "", cfg.target_zone, "");
    return;
  }

  char origin[TZ_NAME_MAX];
  rewrite_origin_zone((const char *)payload, payload_length, origin, sizeof(origin));

  char *rewritten = NULL;
  size_t rewritten_length = 0;
  rewrite_context used;
  memset(&used, 0, sizeof(used));
  rewrite_status status = rewrite_payload((const char *)payload, payload_length, &cfg,
                                          plugin_now(), &rewritten, &rewritten_length, &used);
  if (status != REWRITE_APPLIED) {
    note_event(&cfg, status == REWRITE_NONE ? "unchanged" : "failed",
               origin, cfg.target_zone, status == REWRITE_NONE ? used.date : "");
    free(payload);
    return;
  }

  char *encoded = base64_encode((unsigned char *)rewritten, rewritten_length);
  free(rewritten);
  free(payload);
  if (!encoded) {
    note_event(&cfg, "failed", origin, cfg.target_zone, used.date);
    return;
  }
  size_t capacity = strlen(encoded) + 64;
  char *result = (char *)malloc(capacity);
  if (result) {
    snprintf(result, capacity, "{\"ok\":true,\"result\":{\"Body\":\"%s\"}}", encoded);
    reply(response, result);
    free(result);
  }
  note_event(&cfg, response->ptr ? "rewritten" : "failed", origin, cfg.target_zone, used.date);
  free(encoded);
}

// ---------------------------------------------------------------------------
// Management API
// ---------------------------------------------------------------------------

static void handle_status(plugin_buffer *out) {
  tz_settings cfg;
  settings_load(&cfg);
  long long now = plugin_now();
  rewrite_context local, target;
  int local_ok = rewrite_context_build(cfg.local_zone, now, &local);
  int target_ok = rewrite_context_build(cfg.target_zone, now, &target);
  char config_file[1200];
  if (!settings_config_path(config_file, sizeof(config_file))) config_file[0] = '\0';
  char *quoted_path = json_encode(config_file);
  // Sized for the worst case: a long escaped path plus both timezone readouts.
  char body[4096];
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"version\":\"%s\",\"settings\":{\"enabled\":%s,\"logging\":%s,"
           "\"local_zone\":\"%s\",\"target_zone\":\"%s\"},"
           "\"config_file\":%s,\"config_writable\":%s,"
           "\"local\":{\"zone\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"abbreviation\":\"%s\",\"offset\":\"%s\"},"
           "\"target\":{\"zone\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"abbreviation\":\"%s\",\"offset\":\"%s\"},"
           "\"timezones\":%d}",
           VERSION,
           cfg.enabled ? "true" : "false", cfg.logging ? "true" : "false",
           cfg.local_zone, cfg.target_zone,
           quoted_path ? quoted_path : "\"\"",
           settings_config_writable() ? "true" : "false",
           local_ok ? local.zone : "", local_ok ? local.date : "", local_ok ? local.time : "",
           local_ok ? local.abbr : "", local_ok ? local.offset : "",
           target_ok ? target.zone : "", target_ok ? target.date : "", target_ok ? target.time : "",
           target_ok ? target.abbr : "", target_ok ? target.offset : "",
           tz_count());
  free(quoted_path);
  json_response(out, 200, body);
}

// One row per zone, ordered as in the generated table (alphabetical).
static void handle_timezones(plugin_buffer *out) {
  size_t capacity = (size_t)tz_count() * 96 + 64;
  char *body = (char *)malloc(capacity);
  if (!body) return;
  size_t used = (size_t)snprintf(body, capacity, "{\"ok\":true,\"zones\":[");
  for (int i = 0; i < tz_count(); i++) {
    const tz_entry *entry = tz_at(i);
    char *quoted = json_encode(entry->name);
    if (!quoted) break;
    int written = snprintf(body + used, capacity - used, "%s%s", i ? "," : "", quoted);
    free(quoted);
    if (written < 0 || (size_t)written >= capacity - used) break;
    used += (size_t)written;
  }
  snprintf(body + used, capacity - used, "]}");
  json_response(out, 200, body);
  free(body);
}

static void handle_events(plugin_buffer *out) {
  char list[EVENT_JSON_BYTES];
  events_json(list, sizeof(list));
  size_t capacity = strlen(list) + 64;
  char *body = (char *)malloc(capacity);
  if (!body) return;
  snprintf(body, capacity, "{\"ok\":true,\"events\":[%s]}", list);
  json_response(out, 200, body);
  free(body);
}

// Dry-run self test: rewrite a synthetic request and report what changed.
static void handle_selftest(plugin_buffer *out) {
  tz_settings cfg;
  settings_load(&cfg);
  if (!cfg.enabled) {
    json_response(out, 409, "{\"ok\":false,\"reason\":\"时区同步已关闭\"}");
    return;
  }
  static const char sample[] =
      "{\"model\":\"codex-selftest\",\"input\":[{\"type\":\"message\",\"role\":\"user\","
      "\"content\":[{\"type\":\"input_text\",\"text\":\"<environment_context><timezone>"
      "Asia/Shanghai</timezone><current_date>2000-01-01</current_date></environment_context>\"}]}]}";
  char *rewritten = NULL;
  size_t rewritten_length = 0;
  rewrite_context used;
  rewrite_status status = rewrite_payload(sample, sizeof(sample) - 1, &cfg, plugin_now(),
                                          &rewritten, &rewritten_length, &used);
  if (status != REWRITE_APPLIED || !rewritten) {
    json_response(out, 500, "{\"ok\":false,\"reason\":\"请求重写自检失败\"}");
    free(rewritten);
    return;
  }
  // The sample is returned as a JSON string, so the response stays valid JSON
  // no matter what the rewritten request contains.
  char *encoded = json_encode(rewritten);
  free(rewritten);
  if (!encoded) return;
  size_t capacity = strlen(encoded) + 256;
  char *body = (char *)malloc(capacity);
  if (body) {
    snprintf(body, capacity, "{\"ok\":true,\"target_zone\":\"%s\",\"date\":\"%s\",\"sample\":%s}",
             used.zone, used.date, encoded);
    json_response(out, 200, body);
    free(body);
  }
  free(encoded);
}

static int json_bool(const char *raw, json_token token, int *out) {
  if (token.type != JSON_PRIMITIVE) return 0;
  size_t length = (size_t)(token.end - token.start);
  if (length == 4 && memcmp(raw + token.start, "true", 4) == 0) { *out = 1; return 1; }
  if (length == 5 && memcmp(raw + token.start, "false", 5) == 0) { *out = 0; return 1; }
  return 0;
}

static void handle_settings_save(const char *raw, const json_token *tokens, int body,
                                 plugin_buffer *out) {
  if (body < 0 || tokens[body].type != JSON_STRING) {
    json_response(out, 400, "{\"ok\":false,\"reason\":\"缺少 JSON 请求体\"}");
    return;
  }
  size_t length = 0;
  unsigned char *decoded = base64_decode(raw + tokens[body].start,
                                         (size_t)(tokens[body].end - tokens[body].start),
                                         &length);
  if (!decoded || length > 2048) {
    free(decoded);
    json_response(out, 400, "{\"ok\":false,\"reason\":\"请求体无效\"}");
    return;
  }
  tz_settings cfg;
  settings_load(&cfg);
  json_token fields[32];
  int field_count = json_parse((char *)decoded, length, fields, 32);
  if (field_count < 1 || fields[0].type != JSON_OBJECT) {
    free(decoded);
    json_response(out, 400, "{\"ok\":false,\"reason\":\"请求体无效\"}");
    return;
  }
  int ok = 1;
  int index = json_field((char *)decoded, fields, field_count, 0, "enabled");
  if (index >= 0 && !json_bool((char *)decoded, fields[index], &cfg.enabled)) ok = 0;
  index = json_field((char *)decoded, fields, field_count, 0, "logging");
  if (index >= 0 && !json_bool((char *)decoded, fields[index], &cfg.logging)) ok = 0;
  const char *names[] = {"local_zone", "target_zone"};
  char *targets[] = {cfg.local_zone, cfg.target_zone};
  const size_t caps[] = {sizeof(cfg.local_zone), sizeof(cfg.target_zone)};
  for (int i = 0; ok && i < 2; i++) {
    index = json_field((char *)decoded, fields, field_count, 0, names[i]);
    if (index < 0) continue;
    if (fields[index].type != JSON_STRING) { ok = 0; break; }
    char *value = json_decode((char *)decoded, fields[index]);
    if (!value || !settings_zone_valid(value) || strlen(value) >= caps[i]) ok = 0;
    else snprintf(targets[i], caps[i], "%s", value);
    free(value);
  }
  free(decoded);
  if (!ok) {
    json_response(out, 400, "{\"ok\":false,\"reason\":\"时区或开关取值无效\"}");
    return;
  }
  if (!settings_save(&cfg)) {
    json_response(out, 500, "{\"ok\":false,\"reason\":\"配置写入失败\"}");
    return;
  }
  handle_status(out);
}

// Routes are matched against the path suffix after ROUTE_PREFIX.
static int route_is(const char *raw, json_token token, const char *suffix) {
  char expected[128];
  snprintf(expected, sizeof(expected), "%s%s", ROUTE_PREFIX, suffix);
  return json_string_is(raw, token, expected);
}

static void handle_management(const unsigned char *request, size_t length, plugin_buffer *response) {
  if (!request || !length || length > BODY_MAX) {
    reply(response, "{\"ok\":false,\"error\":{\"code\":\"invalid_request\"}}");
    return;
  }
  const char *raw = (const char *)request;
  json_token *tokens = (json_token *)malloc(TOKENS_MAX * sizeof(json_token));
  if (!tokens) return;
  int count = json_parse(raw, length, tokens, TOKENS_MAX);
  if (count < 1 || tokens[0].type != JSON_OBJECT) {
    free(tokens);
    return;
  }
  int object = 0;
  int wrapped = json_field(raw, tokens, count, object, "request");
  if (wrapped >= 0 && tokens[wrapped].type == JSON_OBJECT) object = wrapped;
  const char *const kPath[] = {"Path", "path", NULL};
  const char *const kMethod[] = {"Method", "method", NULL};
  int path = json_field_any(raw, tokens, count, object, kPath);
  int method = json_field_any(raw, tokens, count, object, kMethod);
  int body = json_field_any(raw, tokens, count, object, kBody);
  if (path < 0 || method < 0) {
    free(tokens);
    reply(response, "{\"ok\":false,\"error\":{\"code\":\"invalid_request\"}}");
    return;
  }

  int is_get = json_string_is(raw, tokens[method], "GET");
  int is_post = json_string_is(raw, tokens[method], "POST");
  int is_put = json_string_is(raw, tokens[method], "PUT");

  if (is_get && json_string_is(raw, tokens[path], PANEL_ROUTE)) {
    free(tokens);
    html_response(response, dashboard_html, dashboard_html_size);
    return;
  }
  if (is_get && route_is(raw, tokens[path], "status")) {
    free(tokens);
    handle_status(response);
    return;
  }
  if (is_get && route_is(raw, tokens[path], "timezones")) {
    free(tokens);
    handle_timezones(response);
    return;
  }
  if (is_get && route_is(raw, tokens[path], "events")) {
    free(tokens);
    handle_events(response);
    return;
  }
  if (is_post && route_is(raw, tokens[path], "events/clear")) {
    events_clear();
    free(tokens);
    json_response(response, 200, "{\"ok\":true}");
    return;
  }
  if (is_post && route_is(raw, tokens[path], "selftest")) {
    free(tokens);
    handle_selftest(response);
    return;
  }
  if (is_put && route_is(raw, tokens[path], "settings")) {
    handle_settings_save(raw, tokens, body, response);
    free(tokens);
    return;
  }
  free(tokens);
  reply(response, "{\"ok\":true,\"result\":{\"StatusCode\":404,\"Headers\":{"
                  "\"Content-Type\":[\"text/plain; charset=utf-8\"]},\"Body\":\"Tm90IGZvdW5k\"}}");
}

// ---------------------------------------------------------------------------
// ABI entry points
// ---------------------------------------------------------------------------

static const char *const kRegister =
    "{\"ok\":true,\"result\":{\"schema_version\":1,\"metadata\":{"
    "\"Name\":\"Codex 离线时区同步\",\"Version\":\"" VERSION "\","
    "\"Author\":\"azmiao\",\"GitHubRepository\":\"https://github.com/azmiao/codex-timezone-sync-local\","
    "\"ConfigFields\":[]},"
    "\"capabilities\":{\"request_interceptor\":true,\"management_api\":true}}}";

static const char *const kManagement =
    "{\"ok\":true,\"result\":{\"Routes\":["
    "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync-local/status\"},"
    "{\"Method\":\"PUT\",\"Path\":\"/plugins/codex-timezone-sync-local/settings\"},"
    "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync-local/timezones\"},"
    "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync-local/events\"},"
    "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync-local/events/clear\"},"
    "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync-local/selftest\"}],"
    "\"Resources\":[{\"Path\":\"/panel\",\"Menu\":\"Codex 离线时区同步\","
    "\"Description\":\"选择本地与目标时区，校正 Codex 请求中的日期与时区\"}]}}";

static int plugin_call(const char *method, const unsigned char *request, size_t length,
                       plugin_buffer *response) {
  if (response) {
    response->ptr = NULL;
    response->len = 0;
  }
  if (!method || !response) return 1;
  if (!strcmp(method, "plugin.register") || !strcmp(method, "plugin.reconfigure")) {
    reply(response, kRegister);
    return 0;
  }
  if (!strcmp(method, "management.register")) {
    reply(response, kManagement);
    return 0;
  }
  if (!strcmp(method, "management.handle")) {
    handle_management(request, length, response);
    return 0;
  }
  if (!strcmp(method, "request.intercept_after")) {
    handle_request(request, length, response);
    if (!response->ptr) reply(response, "{\"ok\":true,\"result\":{}}");
    return 0;
  }
  if (!strcmp(method, "request.intercept_before")) {
    reply(response, "{\"ok\":true,\"result\":{}}");
    return 0;
  }
  reply(response, "{\"ok\":false,\"error\":{\"code\":\"unknown_method\","
                  "\"message\":\"unsupported method\"}}");
  return 0;
}

static void plugin_free(void *pointer, size_t length) {
  (void)length;
  free(pointer);
}

static void plugin_shutdown(void) {
  events_clear();
}

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int cliproxy_plugin_init(void *host, void *plugin) {
  (void)host;
  if (!plugin) return 1;
  cliproxy_plugin_api *api = (cliproxy_plugin_api *)plugin;
  api->abi_version = ABI_VERSION;
  api->call = plugin_call;
  api->free_buffer = plugin_free;
  api->shutdown = plugin_shutdown;
  events_init();
  return 0;
}
