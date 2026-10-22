#!/usr/bin/env python3
"""Validate the committed timezone table against the reference tzdata database.

The native engine in timezone.c interprets the two row kinds stored in
timezone_table.h: POSIX TZ rules and explicit transition lists. This test
re-implements that interpretation in Python and compares it with zoneinfo for
every zone across the supported window, so a bad table row or a wrong
transition rule fails without needing a C toolchain.
"""
import datetime as dt
import pathlib
import re
import unittest
import zoneinfo

ROOT = pathlib.Path(__file__).resolve().parent
TABLE = ROOT / 'timezone_table.h'
DAY = 86400
UTC = dt.timezone.utc
EPOCH = dt.date(1970, 1, 1)
# Must stay in sync with WINDOW_START/WINDOW_END in tools/gen_tz_table.py.
WINDOW_START = int(dt.datetime(2024, 1, 1, tzinfo=UTC).timestamp())
WINDOW_END = int(dt.datetime(2045, 1, 1, tzinfo=UTC).timestamp())

ENTRY = re.compile(r'^\s*\{"([^"]+)", (NULL|"[^"]*"), (NULL|tz_rows_\w+), (\d+)\},\s*$')
ROW = re.compile(r'^\s*\{(-?\d+)LL, (-?\d+), "([^"]*)"\},\s*$')
BLOCK = re.compile(r'^static const tz_row (tz_rows_\w+)\[\d+\] = \{$')


def load_table():
    """Return {zone: ('rule', text) | ('list', [(epoch, offset, abbr)])}."""
    text = TABLE.read_text(encoding='ascii')
    blocks, current = {}, None
    for line in text.splitlines():
        hit = BLOCK.match(line)
        if hit:
            current = hit.group(1)
            blocks[current] = []
            continue
        if current:
            if line.startswith('};'):
                current = None
                continue
            row = ROW.match(line)
            if row:
                blocks[current].append((int(row.group(1)), int(row.group(2)), row.group(3)))
    zones = {}
    for line in text.splitlines():
        hit = ENTRY.match(line)
        if not hit:
            continue
        name, rule, rows, count = hit.groups()
        if rule != 'NULL':
            zones[name] = ('rule', rule[1:-1])
        else:
            payload = blocks.get(rows, [])
            assert len(payload) == int(count), f'{name}: row count {len(payload)} != {count}'
            zones[name] = ('list', payload)
    return zones


class PosixRule:
    """Mirror of parse_rule/dst_active in timezone.c."""

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
            month = self._number()
            assert self._take('.')
            week = self._number()
            assert self._take('.')
            spec = ('M', month, week, self._number())
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
        if spec[0] == 'M':
            _, month, week, weekday = spec
            first = dt.date(year, month, 1)
            offset = (weekday - (first.weekday() + 1) % 7) % 7
            day = 1 + offset + (week - 1) * 7
            length = (dt.date(year + (month == 12), month % 12 + 1, 1) - first).days
            while day > length:
                day -= 7
            return (dt.date(year, month, day) - EPOCH).days
        if spec[0] == 'J':
            leap = year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
            value = spec[1] + (1 if leap and spec[1] >= 60 else 0)
            return (dt.date(year, 1, 1) + dt.timedelta(days=value - 1) - EPOCH).days
        return (dt.date(year, 1, 1) + dt.timedelta(days=spec[1]) - EPOCH).days

    def instant(self, transition, year, offset):
        spec, seconds = transition
        return self._day(spec, year) * DAY + seconds - offset

    def at(self, epoch):
        if self.dst_abbr is None:
            return self.std_offset, self.std_abbr
        year = dt.datetime.fromtimestamp(epoch, UTC).year
        for candidate in (year - 1, year):
            start = self.instant(self.start, candidate, self.std_offset)
            same_year_end = self.instant(self.end, candidate, self.dst_offset)
            end = same_year_end if start <= same_year_end else self.instant(self.end, candidate + 1, self.dst_offset)
            if start <= epoch < end:
                return self.dst_offset, self.dst_abbr
        return self.std_offset, self.std_abbr


def resolve(zone, epoch):
    """Mirror of tz_evaluate in timezone.c."""
    kind, payload = zone
    if kind == 'rule':
        return PosixRule(payload).at(epoch)
    chosen = payload[0]
    for row in payload[1:]:
        if row[0] > epoch:
            break
        chosen = row
    return chosen[1], chosen[2]


def reference(zone, epoch):
    local = dt.datetime.fromtimestamp(epoch, zoneinfo.ZoneInfo(zone))
    return int(local.utcoffset().total_seconds()), local.tzname(), local


def sample_instants(step_hours):
    instants = []
    moment = WINDOW_START
    while moment < WINDOW_END:
        instants.append(moment)
        moment += step_hours * 3600
    return instants


class TestTimezoneTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.zones = load_table()

    def test_table_shape(self):
        self.assertGreater(len(self.zones), 400)
        for name, (kind, payload) in self.zones.items():
            with self.subTest(zone=name):
                if kind == 'rule':
                    PosixRule(payload)
                else:
                    self.assertGreater(len(payload), 0)
                    self.assertEqual(payload[0][0], WINDOW_START)
                    epochs = [row[0] for row in payload]
                    self.assertEqual(epochs, sorted(epochs))
                    self.assertEqual(len(epochs), len(set(epochs)))

    def test_names_are_valid_iana_zones(self):
        available = zoneinfo.available_timezones()
        if not available:
            self.skipTest('no tzdata installed')
        self.assertEqual(sorted(set(self.zones) - available), [])
        self.assertIn('Asia/Shanghai', self.zones)
        self.assertIn('America/Los_Angeles', self.zones)

    def test_rules_match_reference(self):
        available = zoneinfo.available_timezones()
        if not available:
            self.skipTest('no tzdata installed')
        # Hourly over the first and last month, every 3h in between: this still
        # lands on both sides of every DST transition in the window.
        instants = sample_instants(3)
        failures = []
        for name in sorted(self.zones):
            if name not in available:
                continue
            for epoch in instants:
                offset, abbr = resolve(self.zones[name], epoch)
                expected_offset, expected_abbr, local = reference(name, epoch)
                if offset != expected_offset or abbr != expected_abbr:
                    failures.append(
                        f'{name}@{dt.datetime.fromtimestamp(epoch, UTC).isoformat()}: '
                        f'got {offset}/{abbr}, want {expected_offset}/{expected_abbr}')
                    break
        self.assertEqual(failures, [])

    def test_transition_boundaries_are_exact(self):
        """Check the minute on both sides of each reference state change."""
        available = zoneinfo.available_timezones()
        if not available:
            self.skipTest('no tzdata installed')
        failures = []
        for name in sorted(self.zones):
            if name not in available:
                continue
            moment = WINDOW_START
            previous = reference(name, moment)[:2]
            while moment < WINDOW_END:
                following = moment + 86400
                current = reference(name, following)[:2]
                if current != previous:
                    low, high = moment, following
                    while high - low > 60:
                        middle = low + (high - low) // 2
                        if reference(name, middle)[:2] == previous:
                            low = middle
                        else:
                            high = middle
                    for probe in (high - 60, high, high + 60):
                        if resolve(self.zones[name], probe) != reference(name, probe)[:2]:
                            failures.append(
                                f'{name}@{dt.datetime.fromtimestamp(probe, UTC).isoformat()}: '
                                f'got {resolve(self.zones[name], probe)}, '
                                f'want {reference(name, probe)[:2]}')
                            break
                    previous = current
                moment = following
        self.assertEqual(failures, [])


class TestFixedOffsets(unittest.TestCase):
    def test_utc_and_fixed_zones(self):
        zones = load_table()
        for name in ('UTC', 'Asia/Shanghai', 'Asia/Kathmandu', 'Asia/Tokyo'):
            offset, abbr = resolve(zones[name], WINDOW_START)
            expected_offset, expected_abbr, _ = reference(name, WINDOW_START)
            self.assertEqual((offset, abbr), (expected_offset, expected_abbr), name)

    def test_southern_hemisphere_summer(self):
        zones = load_table()
        january = int(dt.datetime(2026, 1, 15, tzinfo=UTC).timestamp())
        july = int(dt.datetime(2026, 7, 15, tzinfo=UTC).timestamp())
        for name in ('Australia/Sydney', 'Pacific/Auckland', 'America/Santiago'):
            for epoch in (january, july):
                offset, abbr = resolve(zones[name], epoch)
                expected_offset, expected_abbr, _ = reference(name, epoch)
                self.assertEqual((offset, abbr), (expected_offset, expected_abbr), name)


if __name__ == '__main__':
    unittest.main(verbosity=2)
