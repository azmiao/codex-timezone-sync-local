#!/usr/bin/env python3
"""Contract tests against the built C ABI plugin (no Codex credentials, no network)."""
import base64
import ctypes as ct
import json
import os
import pathlib
import sys
import tempfile
import unittest
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent
LIBRARY = {
    'win32': 'codex-timezone-sync-local.dll',
    'darwin': 'codex-timezone-sync-local.dylib',
}.get(sys.platform, 'codex-timezone-sync-local.so')


class Buffer(ct.Structure):
    _fields_ = [('ptr', ct.c_void_p), ('len', ct.c_size_t)]


Call = ct.CFUNCTYPE(ct.c_int, ct.c_char_p, ct.c_void_p, ct.c_size_t, ct.POINTER(Buffer))
Free = ct.CFUNCTYPE(None, ct.c_void_p, ct.c_size_t)
Shutdown = ct.CFUNCTYPE(None)


class API(ct.Structure):
    _fields_ = [('abi_version', ct.c_uint32), ('call', Call), ('free_buffer', Free),
                ('shutdown', Shutdown)]


LIB = ct.CDLL(str(ROOT / LIBRARY))
LIB.cliproxy_plugin_init.argtypes = [ct.c_void_p, ct.POINTER(API)]
LIB.cliproxy_plugin_init.restype = ct.c_int
API_OBJ = API()
assert LIB.cliproxy_plugin_init(None, ct.byref(API_OBJ)) == 0
assert API_OBJ.abi_version == 1

PREFIX = '/v0/management/plugins/codex-timezone-sync-local/'
PANEL = '/v0/resource/plugins/codex-timezone-sync-local/panel'
MARKER = '<environment_context>'


def call(method, payload=None):
    encoded = b'' if payload is None else json.dumps(payload, ensure_ascii=False).encode()
    memory = ct.create_string_buffer(encoded)
    response = Buffer()
    status = API_OBJ.call(method.encode(), memory, len(encoded), ct.byref(response))
    assert status == 0, status
    try:
        return json.loads(ct.string_at(response.ptr, response.len))
    finally:
        API_OBJ.free_buffer(response.ptr, response.len)


def encode_body(body):
    return base64.b64encode(json.dumps(body, ensure_ascii=False).encode()).decode()


def decode_body(encoded):
    return json.loads(base64.b64decode(encoded))


def intercept(body, fmt='codex', wrap=False):
    envelope = {'ToFormat': fmt, 'Body': encode_body(body)}
    if wrap:
        envelope = {'request': envelope}
    result = call('request.intercept_after', envelope)
    assert result['ok'], result
    encoded = result.get('result', {}).get('Body')
    return decode_body(encoded) if encoded else None


def environment_text(payload):
    """Flatten every string in a request body so tests can assert on content."""
    found = []

    def walk(node):
        if isinstance(node, str):
            found.append(node)
        elif isinstance(node, list):
            for item in node:
                walk(item)
        elif isinstance(node, dict):
            for item in node.values():
                walk(item)

    walk(payload)
    return '\n'.join(found)


class PluginTestCase(unittest.TestCase):
    """Shared fixture: a temporary config file per test."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.config = pathlib.Path(self.temp.name) / 'codex-timezone-sync-local.config'
        self.config.unlink(missing_ok=True)
        os.environ['CODEX_TZ_CONFIG_FILE'] = str(self.config)

    def tearDown(self):
        os.environ.pop('CODEX_TZ_CONFIG_FILE', None)
        self.temp.cleanup()

    def management(self, endpoint, method='GET', data=None):
        request = {'Path': PREFIX + endpoint, 'Method': method}
        if data is not None:
            request['Body'] = base64.b64encode(json.dumps(data).encode()).decode()
        result = call('management.handle', request)['result']
        return result['StatusCode'], json.loads(base64.b64decode(result['Body']))

    def save(self, **fields):
        code, body = self.management('settings', 'PUT', fields)
        self.assertEqual(code, 200, body)
        return body

    def local_date(self, zone):
        from zoneinfo import ZoneInfo
        return datetime.now(ZoneInfo(zone)).strftime('%Y-%m-%d')


class TestRegistration(PluginTestCase):
    def test_register_declares_interceptor_and_management(self):
        result = call('plugin.register')['result']
        self.assertEqual(result['schema_version'], 1)
        self.assertTrue(result['capabilities']['request_interceptor'])
        self.assertTrue(result['capabilities']['management_api'])
        self.assertEqual(result['metadata']['Name'], 'Codex 离线时区同步')
        self.assertEqual(result['metadata']['Version'], '0.7.0')
        # The store registry and the release workflow carry the same version.
        self.assertEqual(json.loads((ROOT / 'registry.json').read_text(encoding='utf-8'))
                         ['plugins'][0]['version'], result['metadata']['Version'])

    def test_plugin_id_matches_local_variant(self):
        # The host derives the plugin ID from the library file name, so the
        # binary, the routes and the store entry must agree on one ID.
        plugin_id = 'codex-timezone-sync-local'
        self.assertEqual(LIBRARY.rsplit('.', 1)[0], plugin_id)
        self.assertIn(f'/plugins/{plugin_id}/', PREFIX)
        self.assertIn(f'/resource/plugins/{plugin_id}/', PANEL)
        entry = json.loads((ROOT / 'registry.json').read_text(encoding='utf-8'))['plugins'][0]
        self.assertEqual(entry['id'], plugin_id)

    def test_management_routes_are_registered(self):
        routes = call('management.register')['result']['Routes']
        pairs = {(route['Method'], route['Path']) for route in routes}
        for expected in (
            ('GET', '/plugins/codex-timezone-sync-local/status'),
            ('PUT', '/plugins/codex-timezone-sync-local/settings'),
            ('GET', '/plugins/codex-timezone-sync-local/timezones'),
            ('GET', '/plugins/codex-timezone-sync-local/events'),
            ('POST', '/plugins/codex-timezone-sync-local/events/clear'),
            ('POST', '/plugins/codex-timezone-sync-local/selftest'),
        ):
            self.assertIn(expected, pairs)
        resources = call('management.register')['result']['Resources']
        self.assertEqual(resources[0]['Path'], '/panel')

    def test_panel_is_self_contained_html(self):
        result = call('management.handle', {'Method': 'GET', 'Path': PANEL})['result']
        self.assertEqual(result['StatusCode'], 200)
        self.assertEqual(result['Headers']['Content-Type'], ['text/html; charset=utf-8'])
        html = base64.b64decode(result['Body']).decode('utf-8')
        self.assertIn('Codex 离线时区同步', html)
        self.assertIn('<select id="local-zone"', html)
        self.assertIn('<select id="target-zone"', html)
        # No external asset may be required: the panel must work offline.
        self.assertNotIn('http://', html)
        self.assertNotIn('https://', html)
        self.assertNotIn('<script src=', html)
        self.assertNotIn('<link rel="stylesheet"', html)

    def test_panel_follows_host_theme(self):
        html = base64.b64decode(call('management.handle', {'Method': 'GET', 'Path': PANEL})['result']['Body']).decode('utf-8')
        # The panel must not pin itself to one theme: the host can switch at runtime.
        self.assertNotIn('name="color-scheme" content="dark"', html)
        self.assertNotIn(':root{color-scheme:dark', html)
        self.assertIn('name="color-scheme" content="light dark"', html)
        self.assertIn('color-scheme:light', html)
        # Dark tokens exist, but only behind the theme attribute.
        self.assertIn('html[data-theme=dark]{color-scheme:dark', html)
        # Host attribute, host storage key and system preference are all consulted.
        self.assertIn("getAttribute('data-theme')", html)
        self.assertIn('cli-proxy-theme', html)
        self.assertIn('prefers-color-scheme: dark', html)
        self.assertIn('MutationObserver', html)

    def test_unknown_route_is_404(self):
        result = call('management.handle', {'Method': 'GET', 'Path': '/v0/unknown'})['result']
        self.assertEqual(result['StatusCode'], 404)


class TestTimezones(PluginTestCase):
    def test_timezone_list_covers_iana_zones(self):
        code, body = self.management('timezones')
        self.assertEqual(code, 200)
        zones = body['zones']
        self.assertGreater(len(zones), 400)
        for expected in ('UTC', 'Asia/Shanghai', 'America/Los_Angeles', 'Europe/London'):
            self.assertIn(expected, zones)
        self.assertEqual(zones, sorted(zones))

    def test_status_reports_both_zones(self):
        code, body = self.management('status')
        self.assertEqual(code, 200)
        self.assertEqual(body['settings']['local_zone'], 'Asia/Shanghai')
        self.assertEqual(body['settings']['target_zone'], 'America/Los_Angeles')
        self.assertTrue(body['settings']['enabled'])
        self.assertTrue(body['local']['offset'].startswith(('+', '-')))
        self.assertTrue(body['target']['offset'].startswith(('+', '-')))
        self.assertGreater(body['timezones'], 400)


class TestSettings(PluginTestCase):
    def test_saved_settings_persist_and_reject_unknown_zones(self):
        self.save(enabled=True, logging=False, local_zone='Asia/Tokyo', target_zone='Europe/Berlin')
        code, body = self.management('status')
        self.assertEqual(code, 200)
        self.assertEqual(body['settings']['local_zone'], 'Asia/Tokyo')
        self.assertEqual(body['settings']['target_zone'], 'Europe/Berlin')
        self.assertFalse(body['settings']['logging'])
        self.assertTrue(self.config.is_file())

        code, body = self.management('settings', 'PUT', {'target_zone': 'Mars/Olympus'})
        self.assertEqual(code, 400)
        self.assertIn('reason', body)
        # The rejected save must not have modified the stored configuration.
        self.assertEqual(self.management('status')[1]['settings']['target_zone'], 'Europe/Berlin')

    def test_malformed_settings_body_is_rejected(self):
        code, _ = self.management('settings', 'PUT', {'enabled': 'yes'})
        self.assertEqual(code, 400)
        code, _ = self.management('settings', 'PUT', {'local_zone': 42})
        self.assertEqual(code, 400)


class TestRewrite(PluginTestCase):
    def test_request_without_environment_gets_context(self):
        result = intercept({'input': []})
        self.assertIsNotNone(result)
        text = environment_text(result)
        self.assertIn(MARKER, text)
        self.assertIn('America/Los_Angeles', text)
        self.assertIn(self.local_date('America/Los_Angeles'), text)

    def test_existing_environment_is_normalized_in_place(self):
        payload = {
            'model': 'gpt-5-codex',
            'input': [{
                'type': 'message',
                'role': 'user',
                'content': [{
                    'type': 'input_text',
                    'text': '<environment_context><timezone>Asia/Shanghai</timezone>'
                            '<current_date>2000-01-01</current_date></environment_context>',
                }],
            }],
        }
        result = intercept(payload)
        text = environment_text(result)
        self.assertIn('America/Los_Angeles', text)
        self.assertNotIn('Asia/Shanghai', text)
        self.assertNotIn('2000-01-01', text)
        # Unrelated fields must survive byte-for-byte.
        self.assertEqual(result['model'], 'gpt-5-codex')
        self.assertEqual(result['input'][0]['role'], 'user')

    def test_instructions_string_gets_suffix(self):
        result = intercept({'instructions': 'Base prompt.', 'input': []})
        self.assertTrue(result['instructions'].startswith('Base prompt.'))
        self.assertIn(MARKER, result['instructions'])

    def test_null_instructions_becomes_context(self):
        result = intercept({'instructions': None, 'input': []})
        self.assertIsInstance(result['instructions'], str)
        self.assertIn(MARKER, result['instructions'])

    def test_chat_completions_messages_get_developer_message(self):
        payload = {'messages': [{'role': 'user', 'content': 'hello'}]}
        result = intercept(payload)
        self.assertEqual(result['messages'][0]['role'], 'developer')
        self.assertIn(MARKER, result['messages'][0]['content'])
        self.assertEqual(result['messages'][1], {'role': 'user', 'content': 'hello'})

    def test_local_zone_is_replaced_by_target_zone(self):
        self.save(local_zone='Asia/Tokyo', target_zone='Europe/Berlin')
        payload = {
            'input': [{
                'type': 'message',
                'role': 'user',
                'content': [{
                    'type': 'input_text',
                    'text': '<environment_context><timezone>Asia/Tokyo</timezone>'
                            '<current_date>1999-12-31</current_date></environment_context>',
                }],
            }],
        }
        result = intercept(payload)
        text = environment_text(result)
        self.assertIn('Europe/Berlin', text)
        self.assertIn(self.local_date('Europe/Berlin'), text)
        self.assertNotIn('Asia/Tokyo', text)

    def test_non_codex_requests_are_untouched(self):
        self.assertIsNone(intercept({'input': []}, fmt='openai'))
        self.assertIsNone(intercept({'input': []}, fmt='claude'))

    def test_disabled_plugin_leaves_requests_alone(self):
        self.save(enabled=False)
        self.assertIsNone(intercept({'input': []}))

    def test_invalid_base64_is_not_fatal(self):
        result = call('request.intercept_after', {'ToFormat': 'codex', 'Body': 'not-base64!!'})
        self.assertTrue(result['ok'])
        self.assertEqual(result['result'], {})

    def test_wrapped_envelope_is_supported(self):
        result = intercept({'input': []}, wrap=True)
        self.assertIn(MARKER, environment_text(result))

    def test_already_correct_request_is_reported_unchanged(self):
        first = intercept({'input': []})
        self.assertIn(MARKER, environment_text(first))
        # A second pass over the same body must not add a duplicate context.
        self.assertIsNone(intercept(first))


class TestActivityLog(PluginTestCase):
    def test_events_record_and_clear(self):
        self.save(logging=True)
        self.management('events/clear', 'POST', {})
        self.assertEqual(self.management('events')[1]['events'], [])
        intercept({'input': []})
        events = self.management('events')[1]['events']
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['result'], 'rewritten')
        self.assertEqual(events[0]['timezone_to'], 'America/Los_Angeles')
        self.assertRegex(events[0]['date'], r'^\d{4}-\d{2}-\d{2}$')
        # The ring must not retain any request text.
        self.assertNotIn('environment_context', json.dumps(events))
        self.assertEqual(self.management('events/clear', 'POST', {})[0], 200)
        self.assertEqual(self.management('events')[1]['events'], [])

    def test_events_are_bounded(self):
        self.management('events/clear', 'POST', {})
        for _ in range(30):
            intercept({'input': []})
        self.assertEqual(len(self.management('events')[1]['events']), 20)


class TestSelfTest(PluginTestCase):
    def test_selftest_rewrites_sample(self):
        code, body = self.management('selftest', 'POST', {})
        self.assertEqual(code, 200, body)
        self.assertEqual(body['target_zone'], 'America/Los_Angeles')
        self.assertIn(MARKER, body['sample'])
        self.assertIn('America/Los_Angeles', body['sample'])

    def test_selftest_reports_disabled_plugin(self):
        self.save(enabled=False)
        code, body = self.management('selftest', 'POST', {})
        self.assertEqual(code, 409)
        self.assertFalse(body['ok'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
