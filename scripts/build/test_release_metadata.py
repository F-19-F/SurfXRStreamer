"""Release inputs and complete artifact publication, without network or publishing."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from release_version import ROOT, product_version, validate
sys.path.insert(0, str(ROOT / "scripts/ci"))
import publish_release


class ReleaseMetadataTests(unittest.TestCase):
    def test_semver_and_override_do_not_modify_wire_contract(self):
        for value in ("1.2.3", "1.2.3-rc.1", "1.2.3+build.5"):
            self.assertEqual(validate(value), value)
        for value in ("v1.2.3", "01.2.3", "1.2", "1.2.3-01", "1.2.3\nother=x", "$(command)"):
            with self.assertRaises(ValueError):
                validate(value)
        with patch.dict(os.environ, SURFXR_RELEASE_VERSION="1.8.0-rc.2"):
            self.assertEqual(product_version(), "1.8.0-rc.2")

    def test_artifact_completeness_and_hashes(self):
        directory = ROOT / "build/tmp"
        directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=directory) as temporary:
            root = Path(temporary)
            for name in publish_release.ASSETS:
                (root / name).write_bytes(b"package")
                (root / (name + ".sha256")).write_text(hashlib.sha256(b"package").hexdigest() + "  " + name)
            self.assertEqual(len(publish_release.verified_assets(root)), 6)
            (root / publish_release.ASSETS[0]).write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "checksum"):
                publish_release.verified_assets(root)

    def test_release_pins_target_and_prerelease_without_overwriting_tag(self):
        environment = {"SURFXR_RELEASE_VERSION": "1.8.0-rc.2", "RELEASE_PRERELEASE": "true",
                       "GITHUB_REPOSITORY": "fixture/project", "GH_TOKEN": "fixture",
                       "GITHUB_SHA": "1" * 40}
        missing = HTTPError("https://example.invalid", 404, "missing", {}, None)
        with patch.dict(os.environ, environment), \
             patch.object(publish_release, "verified_assets", return_value=["package.zip"]), \
             patch.object(publish_release.urllib.request, "urlopen", side_effect=missing), \
             patch.object(publish_release.subprocess, "run") as run:
            publish_release.main()
            argv = run.call_args.args[0]
            self.assertIn("--prerelease=true", argv)
            self.assertIn("--latest=false", argv)
            self.assertEqual(argv[argv.index("--target") + 1], "1" * 40)
        with patch.dict(os.environ, {**environment, "RELEASE_PRERELEASE": "false"}), \
             patch.object(publish_release.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "pre-release"):
                publish_release.main()
            run.assert_not_called()
        with patch.dict(os.environ, environment), \
             patch.object(publish_release, "verified_assets", return_value=[]), \
             patch.object(publish_release.urllib.request, "urlopen"), \
             patch.object(publish_release.json, "load", return_value={}), \
             patch.object(publish_release.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "already exists"):
                publish_release.main()
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
