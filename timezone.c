// Offline IANA timezone evaluation from POSIX TZ rules.
// No host zoneinfo lookup, no tzdata package, no network access.
#include "timezone.h"
#include "timezone_table.h"

#include <stdio.h>
#include <string.h>

#define TZ_RULE_MAX 128
#define DAY 86400LL

enum { TR_NONE = 0, TR_JULIAN_NO_LEAP = 1, TR_JULIAN = 2, TR_MONTH_WEEK_DAY = 3 };

typedef struct {
  int kind;
  int month;
  int week;
  int day;
  int day_of_year;
  int seconds; // seconds after local midnight
} tz_transition;

typedef struct {
  char std_abbr[24];
  int std_offset; // seconds east of UTC
  char dst_abbr[24];
  int dst_offset;
  int has_dst;
  tz_transition start;
  tz_transition end;
} tz_rule;

static int is_leap(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month) {
  static const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && is_leap(year) ? 29 : table[month - 1];
}

// Howard Hinnant's civil calendar algorithms; valid for the whole Unix range.
static long long days_from_civil(int year, int month, int day) {
  long long y = year - (month <= 2);
  long long era = (y >= 0 ? y : y - 399) / 400;
  long long yoe = y - era * 400;
  long long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civil_from_days(long long days, int *year, int *month, int *day) {
  long long z = days + 719468;
  long long era = (z >= 0 ? z : z - 146096) / 146097;
  long long doe = z - era * 146097;
  long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long long mp = (5 * doy + 2) / 153;
  *day = (int)(doy - (153 * mp + 2) / 5 + 1);
  *month = (int)(mp < 10 ? mp + 3 : mp - 9);
  *year = (int)(yoe + era * 400 + (*month <= 2));
}

static long long floor_days(long long seconds) {
  return seconds >= 0 ? seconds / DAY : -((-seconds + DAY - 1) / DAY);
}

static void doy_to_date(int year, int day_of_year, int *month, int *day) {
  int remaining = day_of_year;
  for (int m = 1; m <= 12; m++) {
    int length = days_in_month(year, m);
    if (remaining <= length) {
      *month = m;
      *day = remaining;
      return;
    }
    remaining -= length;
  }
  *month = 12;
  *day = 31;
}

static int parse_name(const char **cursor, char *out, size_t cap) {
  const char *p = *cursor;
  if (*p == '<') {
    const char *close = strchr(p + 1, '>');
    if (!close) return 0;
    size_t length = (size_t)(close - p - 1);
    if (length == 0 || length >= cap) return 0;
    memcpy(out, p + 1, length);
    out[length] = 0;
    *cursor = close + 1;
    return 1;
  }
  size_t length = 0;
  while (p[length] && ((p[length] >= 'A' && p[length] <= 'Z') || (p[length] >= 'a' && p[length] <= 'z'))) length++;
  if (length < 3 || length >= cap) return 0;
  memcpy(out, p, length);
  out[length] = 0;
  *cursor = p + length;
  return 1;
}

static int parse_number(const char **cursor, int limit, int *out) {
  const char *p = *cursor;
  if (*p < '0' || *p > '9') return 0;
  int value = 0;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    if (value > limit) return 0;
    p++;
  }
  *cursor = p;
  *out = value;
  return 1;
}

// POSIX offsets state the value added to local time to reach UTC, so the
// east-of-UTC offset is the negation of the parsed value.
static int parse_posix_offset(const char **cursor, int *east) {
  const char *p = *cursor;
  int sign = 1;
  if (*p == '+' || *p == '-') {
    if (*p == '-') sign = -1;
    p++;
  }
  int hour = 0, minute = 0, second = 0;
  if (!parse_number(&p, 168, &hour)) return 0;
  if (*p == ':') {
    p++;
    if (!parse_number(&p, 59, &minute)) return 0;
    if (*p == ':') {
      p++;
      if (!parse_number(&p, 59, &second)) return 0;
    }
  }
  *east = -sign * (hour * 3600 + minute * 60 + second);
  *cursor = p;
  return 1;
}

// Transition times are wall-clock seconds and may be negative or exceed a day.
static int parse_transition_time(const char **cursor, int *seconds) {
  const char *p = *cursor;
  int sign = 1;
  if (*p == '+' || *p == '-') {
    if (*p == '-') sign = -1;
    p++;
  }
  int hour = 0, minute = 0, second = 0;
  if (!parse_number(&p, 167, &hour)) return 0;
  if (*p == ':') {
    p++;
    if (!parse_number(&p, 59, &minute)) return 0;
    if (*p == ':') {
      p++;
      if (!parse_number(&p, 59, &second)) return 0;
    }
  }
  *seconds = sign * (hour * 3600 + minute * 60 + second);
  *cursor = p;
  return 1;
}

static int parse_transition(const char **cursor, tz_transition *out) {
  memset(out, 0, sizeof(*out));
  const char *p = *cursor;
  if (*p == 'M') {
    p++;
    int month = 0, week = 0, day = 0;
    if (!parse_number(&p, 12, &month) || *p != '.') return 0;
    p++;
    if (!parse_number(&p, 5, &week) || *p != '.') return 0;
    p++;
    if (!parse_number(&p, 6, &day)) return 0;
    if (month < 1 || week < 1 || day > 6) return 0;
    out->kind = TR_MONTH_WEEK_DAY;
    out->month = month;
    out->week = week;
    out->day = day;
  } else if (*p == 'J') {
    p++;
    int value = 0;
    if (!parse_number(&p, 365, &value) || value < 1) return 0;
    out->kind = TR_JULIAN_NO_LEAP;
    out->day_of_year = value;
  } else {
    int value = 0;
    if (!parse_number(&p, 365, &value)) return 0;
    out->kind = TR_JULIAN;
    out->day_of_year = value;
  }
  out->seconds = 7200; // POSIX default transition time is 02:00:00.
  if (*p == '/') {
    p++;
    if (!parse_transition_time(&p, &out->seconds)) return 0;
  }
  *cursor = p;
  return 1;
}

static int parse_rule(const char *text, tz_rule *out) {
  memset(out, 0, sizeof(*out));
  if (!text || !*text || strlen(text) >= TZ_RULE_MAX) return 0;
  const char *p = text;
  if (!parse_name(&p, out->std_abbr, sizeof(out->std_abbr))) return 0;
  if (!parse_posix_offset(&p, &out->std_offset)) return 0;
  if (!*p) return 1;
  if (!parse_name(&p, out->dst_abbr, sizeof(out->dst_abbr))) return 0;
  out->dst_offset = out->std_offset + 3600;
  if (*p && *p != ',') {
    if (!parse_posix_offset(&p, &out->dst_offset)) return 0;
  }
  if (*p != ',') return 0;
  p++;
  if (!parse_transition(&p, &out->start)) return 0;
  if (*p != ',') return 0;
  p++;
  if (!parse_transition(&p, &out->end)) return 0;
  if (*p) return 0;
  out->has_dst = 1;
  return 1;
}

// Convert a rule transition into the UTC instant for the given year.
static long long transition_epoch(const tz_transition *transition, int year, int offset_east) {
  int month = 0, day = 0;
  if (transition->kind == TR_MONTH_WEEK_DAY) {
    long long first = days_from_civil(year, transition->month, 1);
    int weekday_first = (int)(((first + 4) % 7 + 7) % 7); // 0 = Sunday
    int day_of_month = 1 + ((transition->day - weekday_first + 7) % 7) + (transition->week - 1) * 7;
    int length = days_in_month(year, transition->month);
    while (day_of_month > length) day_of_month -= 7;
    month = transition->month;
    day = day_of_month;
  } else if (transition->kind == TR_JULIAN_NO_LEAP) {
    int value = transition->day_of_year;
    if (is_leap(year) && value >= 60) value++; // J60 is always March 1.
    doy_to_date(year, value, &month, &day);
  } else if (transition->kind == TR_JULIAN) {
    doy_to_date(year, transition->day_of_year + 1, &month, &day);
  } else {
    return 0;
  }
  return days_from_civil(year, month, day) * DAY + transition->seconds - offset_east;
}

static int dst_active(const tz_rule *rule, long long epoch) {
  if (!rule->has_dst) return 0;
  int year = 0, month = 0, day = 0;
  civil_from_days(floor_days(epoch), &year, &month, &day);
  (void)month;
  (void)day;
  // A window can open in one year and close in the next (southern hemisphere),
  // so each candidate start year is paired with its matching end year.
  for (int candidate = year - 1; candidate <= year; candidate++) {
    long long start = transition_epoch(&rule->start, candidate, rule->std_offset);
    long long same_year_end = transition_epoch(&rule->end, candidate, rule->dst_offset);
    long long end = start <= same_year_end
        ? same_year_end
        : transition_epoch(&rule->end, candidate + 1, rule->dst_offset);
    if (epoch >= start && epoch < end) return 1;
  }
  return 0;
}

int tz_count(void) {
  return (int)(sizeof(tz_table) / sizeof(tz_table[0]));
}

const tz_entry *tz_at(int index) {
  if (index < 0 || index >= tz_count()) return NULL;
  return &tz_table[index];
}

const tz_entry *tz_find(const char *name) {
  if (!name || !*name || strlen(name) >= TZ_NAME_MAX) return NULL;
  for (size_t i = 0; i < sizeof(tz_table) / sizeof(tz_table[0]); i++)
    if (strcmp(tz_table[i].name, name) == 0) return &tz_table[i];
  return NULL;
}

// Resolve a list zone: the last row at or before `epoch` wins. Instants before
// the first row clamp to it, which keeps the result defined outside the window.
static int resolve_rows(const tz_entry *entry, long long epoch, int *offset, const char **abbr) {
  if (!entry->rows || entry->row_count < 1) return 0;
  const tz_row *chosen = &entry->rows[0];
  for (int i = 1; i < entry->row_count; i++) {
    if (entry->rows[i].epoch > epoch) break;
    chosen = &entry->rows[i];
  }
  *offset = chosen->offset;
  *abbr = chosen->abbr;
  return 1;
}

static void compose_local(long long epoch, int offset, tz_now *out) {
  long long local = epoch + offset;
  long long days = floor_days(local);
  long long seconds = local - days * DAY;
  int year = 0, month = 0, day = 0;
  civil_from_days(days, &year, &month, &day);
  snprintf(out->date, sizeof(out->date), "%04d-%02d-%02d", year, month, day);
  snprintf(out->time, sizeof(out->time), "%02d:%02d:%02d",
           (int)(seconds / 3600), (int)(seconds / 60 % 60), (int)(seconds % 60));
}

int tz_evaluate(const char *name, long long epoch, tz_now *out) {
  const tz_entry *entry = tz_find(name);
  if (!entry || !out) return 0;
  int offset = 0;
  const char *abbr = NULL;
  if (entry->rows) {
    if (!resolve_rows(entry, epoch, &offset, &abbr)) return 0;
  } else {
    tz_rule rule;
    if (!parse_rule(entry->rule, &rule)) return 0;
    int dst = dst_active(&rule, epoch);
    offset = dst ? rule.dst_offset : rule.std_offset;
    abbr = dst ? rule.dst_abbr : rule.std_abbr;
  }
  memset(out, 0, sizeof(*out));
  out->utc_offset = offset;
  snprintf(out->abbr, sizeof(out->abbr), "%s", abbr);
  compose_local(epoch, offset, out);
  return 1;
}

void tz_format_offset(int seconds, char *out, size_t cap) {
  if (!out || cap < 7) {
    if (out && cap) out[0] = 0;
    return;
  }
  char sign = seconds < 0 ? '-' : '+';
  int total = seconds < 0 ? -seconds : seconds;
  snprintf(out, cap, "%c%02d:%02d", sign, total / 3600, total / 60 % 60);
}
