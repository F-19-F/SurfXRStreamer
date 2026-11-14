"""Offline checks for the production portable archive and cached hash gates."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile
from release_dependencies import extract_vendor, vendor_archive


class PortableDependencyTests(unittest.TestCase):
    def test_client_subset_keeps_libraries_and_licenses_without_server_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); archive = root/"upstream.zip"
            with zipfile.ZipFile(archive,"w") as z:
                for name in ("ssh.exe","scp.exe","libcrypto.dll","LICENSE.txt","NOTICE.txt","sshd.exe","install-sshd.ps1"):
                    z.writestr("OpenSSH-Win64/"+name,b"upstream-fixture")
            package = {"name":"openssh", "prefix":"OpenSSH-Win64/", "required":("ssh.exe","scp.exe","LICENSE.txt")}
            extract_vendor(archive,root/"tools",package)
            self.assertEqual({p.name for p in (root/"tools").iterdir()},
                             {"ssh.exe","scp.exe","libcrypto.dll","LICENSE.txt","NOTICE.txt"})

    def test_archive_cannot_escape_or_replace_files_with_case_aliases(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); archive = root/"upstream.zip"
            for names in (("../outside.exe",),("C:/outside.exe",),("SSH.exe","ssh.exe")):
                with self.subTest(names=names):
                    with zipfile.ZipFile(archive,"w") as z:
                        for name in names: z.writestr(name,b"fixture")
                    with self.assertRaises(ValueError):
                        extract_vendor(archive,root/"tools",{"name":"python","prefix":"","required":()})
                    self.assertFalse((root/"outside.exe").exists())

    def test_cached_vendor_hash_is_checked_without_network(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); target = root/"fixture-1.zip"; target.write_bytes(b"verified-upstream-fixture")
            package = {"name":"fixture","version":"1","sha256":hashlib.sha256(target.read_bytes()).hexdigest()}
            with mock.patch("release_dependencies.urllib.request.urlopen") as network:
                self.assertEqual(vendor_archive(package,root),target)
                target.write_bytes(b"tampered-fixture")
                with self.assertRaisesRegex(ValueError,"archive_hash_mismatch"):
                    vendor_archive(package,root)
                network.assert_not_called()
