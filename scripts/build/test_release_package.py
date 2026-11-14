"""Offline checks for production release selection and archive integrity."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from unittest import mock
import shutil
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launch"))
import frame_remote
from release_package import assert_arm64_binary, runtime_sources, write_archive
from release_package import package_release
from release_version import product_version


class ReleasePackageTests(unittest.TestCase):
    def test_local_validation_checks_sources_payload_and_executor_without_ssh(self):
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary) / "surfxr"
            project.mkdir()
            (project.parent / "source.cpp").write_bytes(b"source")
            directory = project.parent / "build/frame/local"
            directory.mkdir(parents=True)
            driver = "driver_00_surfxr_exo_input.so"
            (directory / driver).write_bytes(b"driver")
            report = {"success": True, "viewer_build_validated": True,
                      "viewer_offline_self_test_passed": True, "native_launcher_build_validated": True,
                      "build_executor": "local", "build_directory": "build/frame/local",
                      "source_sha256": {"source.cpp": hashlib.sha256(b"source").hexdigest()},
                      "optional_binary_sha256": {driver: hashlib.sha256(b"driver").hexdigest()}}
            report_file = project / "report.json"
            report_file.write_text(json.dumps(report))
            with mock.patch.object(frame_remote, "PROJECT", project), \
                 mock.patch.object(frame_remote, "source_manifest", return_value=["source.cpp"]), \
                 mock.patch.object(frame_remote.subprocess, "run", side_effect=AssertionError("SSH not allowed")):
                self.assertEqual(frame_remote.validated_build(report_file, "local"), str(directory.resolve()))
                with self.assertRaisesRegex(ValueError, "not_validated_for_this_host"):
                    frame_remote.validated_build(report_file, "fixture@offline")
                for invalid in ("build-local", "build/frame/../../outside", "/build/frame/local"):
                    report_file.write_text(json.dumps({**report, "build_directory": invalid}))
                    with self.assertRaisesRegex(ValueError, "invalid_local_build_directory"):
                        frame_remote.validated_build(report_file, "local")
                report_file.write_text(json.dumps(report))
                (directory / driver).write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "optional_frame_payload_changed"):
                    frame_remote.validated_build(report_file, "local")
                (project.parent / "source.cpp").write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "sources_changed"):
                    frame_remote.validated_build(report_file, "local")

    def test_native_runtime_does_not_package_headset_python_sources(self):
        project = Path(__file__).resolve().parents[2] / "surfxr"
        names = runtime_sources(project)
        self.assertEqual(names, [])
        self.assertFalse(any(name.endswith(".py") for name in names))

    def test_full_package_contains_five_native_programs_and_rejects_changed_binary(self):
        project = Path(__file__).resolve().parents[2] / "surfxr"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = bytearray(20)
            header[:6] = b"\x7fELF\x02\x01"
            header[18:20] = (183).to_bytes(2, "little")
            fixtures = root / "fixtures"
            fixtures.mkdir()
            names = ("surfxr-viewer", "surfxr-dashboard", "surfxr-discover", "surfxr-client", "surfxr-runtime-bridge")
            for name in names: (fixtures / name).write_bytes(header)
            runtime_names = ["runtime/lib/lib" + component + ".so.1"
                             for component in ("avcodec", "avutil", "avformat", "swscale")]
            runtime_names.append("runtime/licenses/FFmpeg-LICENSE.txt")
            for name in runtime_names:
                target = fixtures / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(header if name.startswith("runtime/lib/") else b"private-runtime-fixture")
            report = {"architecture": "aarch64", "standalone": True, "build_executor": "local",
                      "runtime_sha256": {n: hashlib.sha256((fixtures/n).read_bytes()).hexdigest() for n in runtime_names}, "native_launcher_build_validated": True,
                      "warning_count": 0, "binary_sha256": {name: hashlib.sha256(header).hexdigest() for name in names}}
            report_file = root / "build.json"
            report_file.write_text(json.dumps(report))
            def command(arguments, **options):
                if arguments[0] == "scp":
                    name = arguments[-2].rsplit("/", 1)[-1]
                    shutil.copyfile(fixtures / name, arguments[-1])
            def git_output(arguments, **options):
                return "fixture-commit" if arguments[1] == "rev-parse" else b""
            def bundle(folder, cache):
                tools = {"ssh":"tools/windows-x64/openssh/ssh.exe", "scp":"tools/windows-x64/openssh/scp.exe",
                         "python":"tools/windows-x64/python/python.exe"}
                for name in tools.values():
                    target = folder/name; target.parent.mkdir(parents=True,exist_ok=True); target.write_bytes(b"fixture-pc-tool")
                return tools
            with mock.patch.object(frame_remote, "validated_build", return_value=str(fixtures)), \
                 mock.patch("release_package.subprocess.run", side_effect=command), \
                 mock.patch("release_package.bundle_pc_tools", side_effect=bundle), \
                 mock.patch("release_package.subprocess.check_output", side_effect=git_output):
                output = root / "fixture-release.zip"
                result = package_release(project, report_file, output, "fixture@offline")
                self.assertFalse(result["frame_python_required"])
                with zipfile.ZipFile(output) as archive:
                    payload = [name for name in archive.namelist() if "/payload/" in name]
                    self.assertEqual(set(payload), {"fixture-release/payload/build/" + name for name in (*names, *runtime_names)})
                    manifest = json.loads(archive.read("fixture-release/release-manifest.json"))
                    self.assertEqual(manifest["format_version"], 2)
                    self.assertEqual(manifest["product_version"], product_version())
                    self.assertEqual(manifest["install_profile"], "surfxr")
                    self.assertFalse(manifest["frame_python_required"])
                    self.assertIn("surfxr/frame/launcher/native/residual_remove.sh.in",manifest["files"])
                    commands = "launch/frame_release_commands.ps1"
                    self.assertIn(commands, manifest["files"])
                    self.assertEqual(hashlib.sha256(archive.read("fixture-release/" + commands)).hexdigest(), manifest["files"][commands])
                    self.assertTrue(result["bundled_pc_tools"])
                    for path in manifest["pc_tools"].values():
                        self.assertIn(path,manifest["files"])
                        self.assertEqual(hashlib.sha256(archive.read("fixture-release/"+path)).hexdigest(),manifest["files"][path])
                driver = "driver_00_surfxr_exo_input.so"
                (fixtures / driver).write_bytes(header)
                report["optional_binary_sha256"] = {driver: hashlib.sha256(header).hexdigest()}
                report_file.write_text(json.dumps(report))
                driver_output = root / "driver-release.zip"
                result = package_release(project, report_file, driver_output, "fixture@offline", "surfxr")
                self.assertEqual(result["precompiled_binaries"], 6)
                with zipfile.ZipFile(driver_output) as archive:
                    manifest = json.loads(archive.read("driver-release/release-manifest.json"))
                    self.assertEqual(manifest["install_profile"], "surfxr")
                    driver_path = "payload/build/" + driver
                    self.assertEqual(hashlib.sha256(archive.read("driver-release/" + driver_path)).hexdigest(),
                                     manifest["files"][driver_path])
                report["optional_binary_sha256"][driver] = "0" * 64
                report_file.write_text(json.dumps(report))
                with self.assertRaisesRegex(ValueError, "release_binary_changed_after_validation"):
                    package_release(project, report_file, root / "rejected-driver.zip", "fixture@offline")
                report["optional_binary_sha256"] = {}
                report["binary_sha256"]["surfxr-client"] = "0" * 64
                report_file.write_text(json.dumps(report))
                with self.assertRaisesRegex(ValueError, "release_binary_changed_after_validation"):
                    package_release(project, report_file, root / "rejected-release.zip", "fixture@offline")

    def test_binary_architecture_and_zip_hash_permissions_roundtrip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            folder = root / "fixture-release"
            binary = folder / "payload/build/surfxr-viewer"
            binary.parent.mkdir(parents=True)
            header = bytearray(20)
            header[:6] = b"\x7fELF\x02\x01"
            header[18:20] = (183).to_bytes(2, "little")
            binary.write_bytes(header)
            assert_arm64_binary(binary)
            digest = hashlib.sha256(header).hexdigest()
            (folder / "release-manifest.json").write_text(json.dumps({"files": {"payload/build/surfxr-viewer": digest}}))
            output = root / "fixture-release.zip"
            checksum = write_archive(folder, output)
            self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), checksum)
            with zipfile.ZipFile(output) as archive:
                info = archive.getinfo("fixture-release/payload/build/surfxr-viewer")
                self.assertEqual((info.external_attr >> 16) & 0o777, 0o755)
                self.assertEqual(hashlib.sha256(archive.read(info)).hexdigest(), digest)
            header[18:20] = (62).to_bytes(2, "little")
            binary.write_bytes(header)
            with self.assertRaisesRegex(ValueError, "release_binary_is_not_arm64_elf"):
                assert_arm64_binary(binary)
