"""Offline checks for the pinned scanner download and cache trust boundary."""
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import prepare_cargo_about as scanner


class ScannerPreparationTests(unittest.TestCase):
    def setUp(self):
        parent = Path(__file__).resolve().parents[2] / "build/tests"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.executable = b"fixture executable, never executed"
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name in (f"{scanner.ASSET}/cargo-about.exe", "../../escaped.txt"):
                member = tarfile.TarInfo(name)
                member.size = len(self.executable)
                archive.addfile(member, io.BytesIO(self.executable))
        self.archive = stream.getvalue()
        for name, value in (("ARCHIVE_SHA256", hashlib.sha256(self.archive).hexdigest()),
                            ("EXE_SHA256", hashlib.sha256(self.executable).hexdigest())):
            patcher = patch.object(scanner, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_verified_download_extracts_only_the_tool_and_reuses_it_offline(self):
        with patch.object(scanner.urllib.request, "urlopen", return_value=io.BytesIO(self.archive)) as request:
            executable = scanner.prepare(self.directory)
            self.assertEqual(executable.read_bytes(), self.executable)
            self.assertEqual(scanner.prepare(self.directory), executable)
            request.assert_called_once()
        self.assertEqual(list(self.directory.iterdir()), [executable])
        self.assertFalse((self.directory.parent / "escaped.txt").exists())

    def test_tampered_cache_is_repaired_from_verified_archive(self):
        executable = self.directory / "cargo-about.exe"
        executable.write_bytes(b"corrupt cache")
        with patch.object(scanner.urllib.request, "urlopen", return_value=io.BytesIO(self.archive)):
            scanner.prepare(self.directory)
        self.assertEqual(executable.read_bytes(), self.executable)

    def test_bad_archive_cannot_replace_existing_tool(self):
        executable = self.directory / "cargo-about.exe"
        executable.write_bytes(b"previous file")
        with patch.object(scanner.urllib.request, "urlopen", return_value=io.BytesIO(b"corrupt download")):
            with self.assertRaisesRegex(RuntimeError, "archive SHA-256 mismatch"):
                scanner.prepare(self.directory)
        self.assertEqual(executable.read_bytes(), b"previous file")

    def test_wrong_executable_is_not_installed(self):
        with patch.object(scanner, "EXE_SHA256", "0" * 64), \
             patch.object(scanner.urllib.request, "urlopen", return_value=io.BytesIO(self.archive)):
            with self.assertRaisesRegex(RuntimeError, "executable SHA-256 mismatch"):
                scanner.prepare(self.directory)
        self.assertFalse((self.directory / "cargo-about.exe").exists())


if __name__ == "__main__":
    unittest.main()
