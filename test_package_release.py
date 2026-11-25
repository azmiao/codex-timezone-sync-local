#!/usr/bin/env python3
"""Packaging contracts using isolated Windows PE-header fixtures."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = pathlib.Path(__file__).resolve().parent / 'package_release.py'
LIBRARY = 'codex-timezone-sync-local.dll'
VERSION = '0.0.1'


class TestWindowsPackaging(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def write_pe_header(self, machine):
        data = bytearray(65537)
        data[:2] = b'MZ'
        data[0x3c:0x40] = (0x80).to_bytes(4, 'little')
        data[0x80:0x84] = b'PE\0\0'
        data[0x84:0x86] = machine.to_bytes(2, 'little')
        (self.root / LIBRARY).write_bytes(data)

    def package(self, platform):
        return subprocess.run(
            [sys.executable, str(SCRIPT), VERSION, platform], cwd=self.root,
            capture_output=True, text=True, check=False,
        )

    def archive_path(self, platform):
        return self.root / 'dist' / f'codex-timezone-sync-local_{VERSION}_{platform}.zip'

    def test_matching_architecture_produces_installable_archive_layout(self):
        for platform, machine in (('windows_amd64', 0x8664), ('windows_arm64', 0xAA64)):
            with self.subTest(platform=platform):
                self.write_pe_header(machine)
                result = self.package(platform)
                self.assertEqual(result.returncode, 0, result.stderr)
                with zipfile.ZipFile(self.archive_path(platform)) as archive:
                    self.assertEqual(archive.namelist(), [LIBRARY])

    def test_wrong_architecture_is_rejected_before_creating_archive(self):
        for platform, machine in (('windows_arm64', 0x8664), ('windows_amd64', 0xAA64)):
            with self.subTest(platform=platform):
                self.write_pe_header(machine)
                result = self.package(platform)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.archive_path(platform).exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
