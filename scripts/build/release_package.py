"""Release archive assembly owned by build_viewer.py; no deployment or media."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from release_dependencies import bundle_pc_tools
from release_version import product_version


def runtime_sources(project: Path) -> list[str]:
    # Services, launcher UI and the shader license are compiled into the native
    # installer from their existing authoritative sources by launcher.cmake.
    return []


def assert_arm64_binary(path: Path) -> None:
    with path.open("rb") as source:
        header = source.read(20)
    if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01"
            or int.from_bytes(header[18:20], "little") != 183):
        raise ValueError("release_binary_is_not_arm64_elf")


def write_archive(folder: Path, output: Path) -> str:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                relative = path.relative_to(folder).as_posix()
                info = zipfile.ZipInfo(folder.name + "/" + relative)
                info.create_system = 3
                executable = relative.startswith("payload/build/surfxr-") or relative.endswith("/surfxr-client")
                info.external_attr = (0o100755 if executable else 0o100644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, path.read_bytes())
    temporary.replace(output)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(digest + "  " + output.name + "\n", encoding="ascii")
    return digest


def package_release(project: Path, report_path: Path, output: Path, host: str, profile: str = "surfxr") -> dict:
    if profile != "surfxr":
        raise ValueError("invalid_release_install_profile")
    sys.path.insert(0, str(project.parent / "scripts/launch"))
    from frame_remote import validated_build
    root = validated_build(report_path, host)
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if (report.get("architecture") != "aarch64" or not report.get("standalone")
            or not report.get("native_launcher_build_validated")):
        raise ValueError("standalone_arm64_validation_required_for_release")
    workspace = project.parent
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=workspace, text=True).strip()
    mappings = {"README.md": project / "docs/RELEASE.md",
                "NotoSans.LICENSE": project / "frame/ui/fonts/NotoSans.LICENSE",
                "LICENSE": workspace / "LICENSE", "NOTICE.md": workspace / "NOTICE.md",
                "Install-Frame.cmd": workspace / "scripts/Install-Frame.cmd",
                "Install-Frame.ps1": workspace / "scripts/Install-Frame.ps1"}
    for name in ("scripts/launch/Start-Frame.ps1", "scripts/launch/frame_release.ps1", "scripts/launch/frame_release_commands.ps1"):
        mappings[name.removeprefix("scripts/")] = workspace / name
    name = "frame/launcher/native/residual_remove.sh.in"
    mappings["surfxr/" + name] = project / name
    for name in runtime_sources(project):
        mappings["payload/source/" + name] = project / name
    paths = sorted({str(path.relative_to(workspace)) for path in mappings.values()})
    from project_manifest import source_manifest
    # Native runtime/embedded assets are no longer copied into the ZIP, but
    # their committed provenance must still match the validated build.
    paths = sorted(set(paths) | set(source_manifest(project)))
    # The parent repository tracks the SDK gitlink, while the validated source
    # manifest still hashes the exact dependency files consumed by the build.
    sdk = "surfxr/pc/surfxr-server/openvr"
    paths = sorted({sdk if name.replace("\\", "/").startswith(sdk + "/") else name for name in paths} | {".gitmodules"})
    changed = subprocess.check_output(["git", "diff", "--ignore-submodules=none", "HEAD", "--", *paths], cwd=workspace)
    if changed:
        raise ValueError("commit_validated_release_sources_before_packaging")
    subprocess.run(["git", "ls-files", "--error-unmatch", "--", *paths], cwd=workspace,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    output = output.resolve()
    if output.suffix.lower() != ".zip" or not re.fullmatch(r"[A-Za-z0-9_.-]+", output.stem):
        raise ValueError("release_output_requires_simple_zip_name")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = workspace / "build/package/frame"
    staging.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="release-", dir=staging) as temporary:
        folder = Path(temporary) / output.stem
        folder.mkdir()
        for name, source in mappings.items():
            target = folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        binary_dir = folder / "payload/build"
        binary_dir.mkdir(parents=True)
        options = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10"]
        def copy_binary(name):
            if report.get("build_executor") == "local":
                shutil.copyfile(Path(root) / name, binary_dir / name)
            else:
                subprocess.run(["scp", *options, host + ":" + root + "/build/" + name, str(binary_dir / name)], check=True)
        for name in ("surfxr-viewer", "surfxr-dashboard", "surfxr-discover", "surfxr-client", "surfxr-runtime-bridge"):
            copy_binary(name)
            assert_arm64_binary(binary_dir / name)
            if hashlib.sha256((binary_dir / name).read_bytes()).hexdigest() != report.get("binary_sha256", {}).get(name):
                raise ValueError("release_binary_changed_after_validation")
        driver_name = "driver_00_surfxr_exo_input.so"
        driver_hash = report.get("optional_binary_sha256", {}).get(driver_name)
        if driver_hash:
            copy_binary(driver_name)
            assert_arm64_binary(binary_dir / driver_name)
            if hashlib.sha256((binary_dir / driver_name).read_bytes()).hexdigest() != driver_hash:
                raise ValueError("release_binary_changed_after_validation")
        runtime = report.get("runtime_sha256", {})
        if any(not any(name.startswith("runtime/lib/lib" + component + ".so.") for name in runtime)
               for component in ("avcodec", "avutil", "avformat", "swscale")):
            raise ValueError("release_requires_private_media_runtime_rebuild_locally")
        for name, expected in runtime.items():
            if not re.fullmatch(r"runtime/(lib/lib[a-z0-9_]+\.so\.[0-9]+|licenses/[A-Za-z0-9_.-]+)", name):
                raise ValueError("invalid_private_runtime_path")
            destination = binary_dir / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            copy_binary(name)
            if hashlib.sha256(destination.read_bytes()).hexdigest() != expected:
                raise ValueError("runtime_changed_after_validation")
            if name.startswith("runtime/lib/"):
                assert_arm64_binary(destination)
        pc_tools = bundle_pc_tools(folder, workspace / "build/deps/release-tools")
        hashes = {path.relative_to(folder).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in folder.rglob("*") if path.is_file()}
        manifest = {"format_version": 2, "product_version": product_version(), "name": output.stem, "source_commit": revision,
                    "architecture": "aarch64", "default_ssh": "steamos@frame",
                    "frame_python_required": False, "install_profile": profile,
                    "pc_tools":pc_tools,
                    "validation": {"arm64_build": True, "offline_ctest": True, "warning_count": report["warning_count"],
                                   "headset_rendering": False, "wearer_acceptance": False}, "files": hashes}
        (folder / "release-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        digest = write_archive(folder, output)
    return {"release": str(output), "sha256": digest, "size_bytes": output.stat().st_size,
            "source_commit": revision, "precompiled_binaries": 5 + bool(driver_hash), "frame_python_required": False,
            "bundled_pc_tools": True, "install_profile": profile,
            "private_configuration_included": False}
