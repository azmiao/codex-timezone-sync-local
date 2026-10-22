#!/usr/bin/env python3
"""Generate timezone_table.h from the IANA tzdata database.

Usage: uv run --with tzdata python tools/gen_tz_table.py

The generated header is committed, so the native plugin needs neither tzdata at
build time nor any host zoneinfo/network access at run time.

Two representations are emitted:

* rule  - the zone's POSIX TZ string (TZif v2+ footer). Compact and valid for
          any instant, so it is used whenever it reproduces the reference data
          across the whole supported window.
* list  - explicit (instant, offset, abbreviation) rows for zones whose future
          behaviour is not a recurring rule, e.g. Ramadan-based Moroccan DST or
          a one-off permanent-offset change. Rows cover the supported window;
          instants outside it clamp to the nearest row.
"""
import datetime as dt
import importlib.resources
import pathlib
import sys
import zoneinfo

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUTPUT = ROOT / 'timezone_table.h'
SKIP = {'Factory', 'localtime', 'posixrules'}
# Supported window. Rules are validated across it; lists enumerate it exactly.
WINDOW_START = dt.datetime(2024, 1, 1, tzinfo=dt.timezone.utc)
WINDOW_END = dt.datetime(2045, 1, 1, tzinfo=dt.timezone.utc)
UTC = dt.timezone.utc


def tzif_root():
    return pathlib.Path(str(importlib.resources.files('tzdata').joinpath('zoneinfo')))


def read_footer(path):
    raw = path.read_bytes()
    if len(raw) < 5 or raw[:4] != b'TZif':
        return None
    if raw[4:5] == b'\x00':
        return ''
    end = raw.rfind(b'\n')
    if end < 0 or end != len(raw) - 1:
        return ''
    start = raw.rfind(b'\n', 0, end)
    if start < 0:
        return ''
    return raw[start + 1:end].decode('ascii')


def abbr_of(local):
    name = local.tzname()
    return name if name else 'UTC'


def fixed_rule(zone, moment):
    """Synthesize `ABBR<offset>` for a zone with a constant offset."""
    local = moment.astimezone(zoneinfo.ZoneInfo(zone))
    total = int(local.utcoffset().total_seconds())
    if total % 60:
        raise ValueError(f'{zone}: sub-minute offset is not representable')
    abbr = abbr_of(local)
    if not (abbr.isalnum() and abbr.isascii()):
        abbr = f'<{abbr}>'
    sign = '+' if total >= 0 else '-'
    total = abs(total)
    hours, rest = divmod(total, 3600)
    minutes, seconds = divmod(rest, 60)
    text = f'{sign}{hours:02d}' if total else '0'
    if minutes or seconds:
        text += f':{minutes:02d}'
    if seconds:
        text += f':{seconds:02d}'
    return f'{abbr}{text}'


def probe(zone, moment):
    local = moment.astimezone(zoneinfo.ZoneInfo(zone))
    return int(local.utcoffset().total_seconds()), abbr_of(local)


def transitions(zone, start, end):
    """Exact state changes (offset or abbreviation) in [start, end)."""
    def state(instant):
        return probe(zone, instant)

    rows = []
    step = dt.timedelta(days=1)
    moment = start
    previous = state(moment)
    while moment < end:
        following = min(moment + step, end)
        current = state(following)
        if current != previous:
            low, high = moment, following
            while (high - low) > dt.timedelta(seconds=1):
                middle = low + (high - low) / 2
                middle = middle.replace(microsecond=0)
                if middle <= low:
                    break
                if state(middle) == previous:
                    low = middle
                else:
                    high = middle
            rows.append(high)
            previous = state(high)
        moment = following
    return rows


def check_rule(zone, text, start, end):
    """True when `text` reproduces the reference offsets across the window.

    Monthly sampling catches zones whose footer only describes the behaviour
    after their last explicit transition (Paraguay 2024, Alberta 2026, Ramadan
    DST). Reference and rule transition instants are then compared exactly, so
    a rule that is right in the middle of a period but wrong at the boundary is
    also rejected.
    """
    rule = PosixRule(text)
    checkpoints = set()
    moment = start
    while moment < end:
        checkpoints.add(int(moment.timestamp()))
        year, month = moment.year, moment.month + 1
        if month == 13:
            year, month = year + 1, 1
        moment = dt.datetime(year, month, 1, tzinfo=UTC)
    for instant in transitions(zone, start, end):
        base = int(instant.timestamp())
        checkpoints.update((base - 1, base, base + 1))
    for year in range(start.year, end.year + 1):
        for transition in (rule.start, rule.end):
            if transition is None:
                continue
            base = rule.instant(transition, year)
            for delta in (-1, 0, 1):
                if start.timestamp() <= base + delta < end.timestamp():
                    checkpoints.add(base + delta)
    for epoch in sorted(checkpoints):
        moment = dt.datetime.fromtimestamp(epoch, UTC)
        if rule.at(epoch) != probe(zone, moment):
            return False
    return True


class PosixRule:
    """Reference POSIX TZ interpreter used only by this generator's validator."""

    def __init__(self, text):
        self.text, self.pos = text, 0
        self.std_abbr = self._name()
        self.std_offset = self._offset()
        self.dst_abbr = self.dst_offset = self.start = self.end = None
        if self.pos < len(text):
            self.dst_abbr = self._name()
            self.dst_offset = self.std_offset + 3600
            if self._peek() not in (',', ''):
                self.dst_offset = self._offset()
            assert self._take(',')
            self.start = self._transition()
            assert self._take(',')
            self.end = self._transition()
            assert self.pos == len(text), text

    def _peek(self):
        return self.text[self.pos] if self.pos < len(self.text) else ''

    def _take(self, char):
        if self._peek() != char:
            return False
        self.pos += 1
        return True

    def _name(self):
        if self._peek() == '<':
            end = self.text.index('>', self.pos)
            value = self.text[self.pos + 1:end]
            self.pos = end + 1
            return value
        start = self.pos
        while self._peek().isascii() and self._peek().isalpha():
            self.pos += 1
        assert self.pos - start >= 3, self.text
        return self.text[start:self.pos]

    def _number(self):
        start = self.pos
        while self._peek().isdigit():
            self.pos += 1
        assert self.pos > start, self.text
        return int(self.text[start:self.pos])

    def _offset(self, signed=False):
        sign = 1
        if self._peek() in ('+', '-'):
            sign = -1 if self._peek() == '-' else 1
            self.pos += 1
        hour = self._number()
        minute = second = 0
        if self._take(':'):
            minute = self._number()
            if self._take(':'):
                second = self._number()
        value = sign * (hour * 3600 + minute * 60 + second)
        return value if signed else -value

    def _transition(self):
        if self._take('M'):
            month, week, weekday = self._number(), None, None
            assert self._take('.')
            week = self._number()
            assert self._take('.')
            weekday = self._number()
            spec = ('M', month, week, weekday)
        elif self._take('J'):
            spec = ('J', self._number())
        else:
            spec = ('N', self._number())
        seconds = 7200
        if self._take('/'):
            seconds = self._offset(signed=True)
        return spec, seconds

    @staticmethod
    def _day(spec, year):
        epoch = dt.date(1970, 1, 1)
        if spec[0] == 'M':
            _, month, week, weekday = spec
            first = dt.date(year, month, 1)
            offset = (weekday - (first.weekday() + 1) % 7) % 7
            day = 1 + offset + (week - 1) * 7
            length = ((dt.date(year + (month == 12), month % 12 + 1, 1)) - first).days
            while day > length:
                day -= 7
            return (dt.date(year, month, day) - epoch).days
        if spec[0] == 'J':
            leap = year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
            value = spec[1] + (1 if leap and spec[1] >= 60 else 0)
            return (dt.date(year, 1, 1) + dt.timedelta(days=value - 1) - epoch).days
        return (dt.date(year, 1, 1) + dt.timedelta(days=spec[1]) - epoch).days

    def instant(self, transition, year):
        spec, seconds = transition
        offset = self.std_offset if transition is self.start else self.dst_offset
        return self._day(spec, year) * 86400 + seconds - offset

    def at(self, epoch):
        if self.dst_abbr is None:
            return self.std_offset, self.std_abbr
        year = dt.datetime.fromtimestamp(epoch, UTC).year
        # A window can open in one year and close in the next (southern
        # hemisphere), so each candidate start year pairs with its own end year.
        for candidate in (year - 1, year):
            start = self.instant(self.start, candidate)
            same_year_end = self.instant(self.end, candidate)
            end = same_year_end if start <= same_year_end else self.instant(self.end, candidate + 1)
            if start <= epoch < end:
                return self.dst_offset, self.dst_abbr
        return self.std_offset, self.std_abbr


def build_rows(zone):
    """Return ('rule', text) or ('list', rows) for one zone."""
    path = tzif_root() / zone
    if not path.is_file():
        return None
    footer = read_footer(path)
    if footer is None:
        return None
    if footer and check_rule(zone, footer, WINDOW_START, WINDOW_END):
        return 'rule', footer
    if not footer and len({probe(zone, moment)[0] for moment in (
            WINDOW_START, WINDOW_START + dt.timedelta(days=183), WINDOW_END)}) == 1:
        return 'rule', fixed_rule(zone, WINDOW_START)
    # Not a recurring rule: enumerate the window exactly.
    rows = [(int(WINDOW_START.timestamp()),) + probe(zone, WINDOW_START)]
    for moment in transitions(zone, WINDOW_START, WINDOW_END):
        rows.append((int(moment.timestamp()),) + probe(zone, moment))
    return 'list', rows


def main():
    available = sorted(zoneinfo.available_timezones())
    if len(available) < 400:
        raise SystemExit('tzdata missing; run with --with tzdata')
    rules, lists = [], []
    for zone in available:
        if zone in SKIP:
            continue
        built = build_rows(zone)
        if built is None:
            continue
        kind, payload = built
        if kind == 'rule':
            if any(c in payload for c in '"\\'):
                raise SystemExit(f'{zone}: rule needs C escaping: {payload!r}')
            rules.append((zone, payload))
        else:
            lists.append((zone, payload))
    if len(rules) + len(lists) < 400:
        raise SystemExit(f'only {len(rules) + len(lists)} zones found')
    write(rules, lists)
    print(f'wrote {OUTPUT.name}: {len(rules)} rule zones, {len(lists)} list zones')
    print(f'list zones: {", ".join(zone for zone, _ in lists)}')
    return 0


def symbol(zone):
    return 'tz_rows_' + ''.join(c if c.isalnum() else '_' for c in zone)


def write(rules, lists):
    lines = [
        '/* Auto-generated by tools/gen_tz_table.py; do not edit.',
        ' * "rule" zones carry a POSIX TZ string valid for any instant.',
        ' * "list" zones carry explicit transitions covering 2024-2045.',
        ' * Offsets and abbreviations are resolved at run time by timezone.c. */',
    ]
    # Row arrays come first: a forward declaration of a const array would be a
    # tentative definition and conflict with the initializer below.
    for zone, rows in lists:
        lines.append(f'static const tz_row {symbol(zone)}[{len(rows)}] = {{')
        for epoch, offset, abbr in rows:
            lines.append(f'  {{{epoch}LL, {offset}, "{abbr}"}},')
        lines.append('};')
        lines.append('')
    lines.append('static const tz_entry tz_table[] = {')
    for zone, rule in rules:
        lines.append(f'  {{"{zone}", "{rule}", NULL, 0}},')
    for zone, rows in lists:
        lines.append(f'  {{"{zone}", NULL, {symbol(zone)}, {len(rows)}}},')
    lines.append('};')
    lines.append('')
    OUTPUT.write_text('\n'.join(lines), encoding='ascii', newline='\n')


if __name__ == '__main__':
    sys.exit(main())
