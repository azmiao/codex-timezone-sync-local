#!/usr/bin/env python3
"""Build a versioned CPA plugin-store archive from the native target DLL/dylib/SO."""
import pathlib
import sys
import zipfile

PLATFORMS = {
    "linux_amd64": "codex-timezone-sync-local.so",
    "linux_arm64": "codex-timezone-sync-local.so",
    "darwin_amd64": "codex-timezone-sync-local.dylib",
    "darwin_arm64": "codex-timezone-sync-local.dylib",
    "windows_amd64": "codex-timezone-sync-local.dll",
    "windows_arm64": "codex-timezone-sync-local.dll",
}

def main():
    if len(sys.argv) != 3 or sys.argv[2] not in PLATFORMS:
        raise SystemExit("Usage: python package_release.py VERSION PLATFORM")
    version, platform = sys.argv[1:]
    if not version or any(c not in "0123456789." for c in version):
        raise SystemExit("Invalid version")
    library = pathlib.Path(PLATFORMS[platform])
    if not library.is_file():
        raise SystemExit(f"Missing compiled binary: {library}")
    if platform.startswith("windows_"):
        with library.open("rb") as binary:
            if binary.read(2) != b"MZ":
                raise SystemExit(f"Not a PE library: {library}")
            binary.seek(0x3c)
            pe_offset = int.from_bytes(binary.read(4), "little")
            binary.seek(pe_offset)
            header = binary.read(6)
        expected = 0xAA64 if platform == "windows_arm64" else 0x8664
        if (len(header) != 6 or header[:4] != b"PE\0\0"
                or int.from_bytes(header[4:], "little") != expected):
            raise SystemExit(f"Expected a {platform} PE library: {library}")
    output = pathlib.Path("dist") / f"codex-timezone-sync-local_{version}_{platform}.zip"
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(library, library.name)
    with zipfile.ZipFile(output) as archive:
        assert archive.namelist() == [library.name]
        # The bundled timezone table makes the library substantially larger than
        # a stub; catch a truncated or empty build here rather than at install time.
        assert archive.getinfo(library.name).file_size > 65536
    print(output)

if __name__ == "__main__":
    main()
