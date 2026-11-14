"""Incrementally build the native viewer and run only offline validation.

Reuses recorded builds; --standalone also works without research evidence.
Never starts XR, streaming, hardware decode or installs runtime packages.
"""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import tempfile
import tarfile

PROJECT = Path(__file__).resolve().parents[2] / "surfxr"
OUTPUT = PROJECT.parent / "build/reports/frame-ssh/validation.json"
HOST = "steamos@frame"
SSH_OPTIONS = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
               "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
from project_manifest import source_manifest
MANIFEST = source_manifest(PROJECT)


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssh", default=HOST)
    parser.add_argument("--local", action="store_true", help="Build on local Linux/WSL without SSH")
    parser.add_argument("--build-directory", type=Path)
    parser.add_argument("--cmake-argument", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--standalone", action="store_true", default=True, help="Build and run offline CTest")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--remote-directory", help="Reuse an existing project-owned Frame build directory")
    parser.add_argument("--build-timeout", type=int, default=900)
    parser.add_argument("--package-release", type=Path, help="Create a precompiled release ZIP from the validated single-link build")
    parser.add_argument("--release-profile", choices=("surfxr",), default="surfxr", help="Select the release installation backend; source deployment defaults stay unchanged")
    parser.add_argument("--package-only", action="store_true", help="Package the current validated --output report without building again")
    args = parser.parse_args()
    if args.local:
        if args.package_only:
            if not args.output or not args.package_release:
                parser.error("--package-only requires --output and --package-release")
            output = args.output
        else:
            from build_local import build
            output = build(PROJECT, args)
        if args.package_release:
            from release_package import package_release
            print(json.dumps(package_release(PROJECT, output, args.package_release, "local", args.release_profile), indent=2))
        return
    if args.build_directory or args.cmake_argument:
        parser.error("--build-directory and --cmake-argument require --local")
    if args.ssh.startswith("-") or not re.fullmatch(r"[A-Za-z0-9_.-]+@[A-Za-z0-9_.:-]+", args.ssh):
        parser.error("--ssh must be user@host without shell syntax")
    if args.build_timeout < 120:
        parser.error("--build-timeout must be at least 120 seconds")
    if args.package_only:
        if not args.package_release or not args.output:
            parser.error("--package-only requires --package-release and --output")
        from release_package import package_release
        print(json.dumps(package_release(PROJECT, args.output, args.package_release, args.ssh, args.release_profile), indent=2))
        return
    output_path = (args.output or OUTPUT).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    baseline = {}
    if output_path.exists():
        previous = json.loads(output_path.read_text(encoding="utf-8"))
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output_path.with_name(output_path.stem + "-before-" + stamp + ".json").write_text(
            json.dumps(previous, indent=2) + "\n", encoding="utf-8")
    else:
        previous = baseline
    if previous.get("ssh_host", HOST) != args.ssh and not args.remote_directory:
        previous = {}
    root = args.remote_directory or previous.get("remote_directory")
    if root and not re.fullmatch(r"/home/[A-Za-z0-9_.-]+/surfxr-(?:research|build)-[A-Za-z0-9_-]+", root):
        raise RuntimeError("Invalid task-owned build directory")
    hashes = {name: hashlib.sha256((PROJECT.parent / name).read_bytes()).hexdigest() for name in MANIFEST}
    report = {"started_utc": now(), "remote_directory": root, "source_project": str(PROJECT),
              "source_sha256": hashes, "commands": [], "success": False,
              "ssh_host": args.ssh, "standalone": args.standalone,
              "scope": "ARM64 source build and existing offline checks; no installation or live session",
              "openxr_session_started": False, "live_discovery_or_session_started": False,
              "viewer_build_validated": False, "viewer_offline_self_test_passed": False}

    def run(argv, *, stdin=None, expected=0, timeout=90):
        record = {"argv": argv, "observed_utc": now(), "expected_exit": expected}
        report["commands"].append(record)
        started = time.monotonic()
        try:
            proc = subprocess.run(argv, input=stdin, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            def text(v):
                return v.decode("utf-8", "replace") if isinstance(v, bytes) else (v or "")
            record.update(timed_out=True, stdout=text(exc.stdout), stderr=text(exc.stderr))
            raise
        finally:
            record["elapsed_seconds"] = round(time.monotonic() - started, 3)
        record.update(returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
        if proc.returncode != expected:
            raise RuntimeError(f"Command exited {proc.returncode}, expected {expected}")
        return proc.stdout

    def remote(argv, **kwargs):
        return run(["ssh", "-T", *SSH_OPTIONS, args.ssh, shlex.join(argv)], **kwargs)

    try:
        if not root:
            # A new checkout needs no local research JSON or captured media.
            root = remote(["python3", "-"], stdin="import pathlib,tempfile\nprint(tempfile.mkdtemp(prefix='surfxr-build-',dir=pathlib.Path.home()))\n").strip()
            if not re.fullmatch(r"/home/[A-Za-z0-9_.-]+/surfxr-build-[A-Za-z0-9_-]+", root):
                raise RuntimeError("Unexpected Frame build directory")
            report["remote_directory"] = root
            remote(["mkdir", "-p", root + "/source"])
        remote(["test", "-d", root + "/source"])
        # Remote hashes reflect partial prior builds as well as completed ones.
        remote_hashes = json.loads(remote(["python3", "-"], stdin=
            "import hashlib,json,pathlib\nroot=pathlib.Path(" + repr(root + "/source") + ")\n"
            "names=" + repr(MANIFEST) + "\n"
            "print(json.dumps({n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in names if (root/n).is_file()}))\n"))
        copied = [name for name in MANIFEST if remote_hashes.get(name) != hashes[name]]
        report["copied_sources"] = copied
        print(f"Frame build: synchronizing {len(copied)} changed source files.", file=sys.stderr, flush=True)
        if copied:
            with tempfile.TemporaryDirectory(prefix="surfxr-source-") as temporary:
                archive = Path(temporary) / "sources.tar.gz"
                with tarfile.open(archive, "w:gz") as tar:
                    for name in copied:
                        tar.add(PROJECT.parent / name, arcname=name, recursive=False)
                run(["scp", *SSH_OPTIONS, str(archive), args.ssh + ":" + root + "/sources.tar.gz"])
                remote(["tar", "-xzf", root + "/sources.tar.gz", "-C", root + "/source"])
        report["architecture"] = remote(["uname", "-m"]).strip()
        remote(["python3", root + "/source/scripts/shaders/compile_foveation_shaders.py", "--check"])
        remote(["python3", root + "/source/scripts/shaders/validate_foveation_gamma2.py"])
        remote(["python3", root + "/source/scripts/shaders/compile_desktop_shaders.py", "--check"])
        remote(["python3", root + "/source/scripts/shaders/compile_image_enhancement_shaders.py", "--check"])
        print("Frame build: configuring and compiling ARM64 targets...", file=sys.stderr, flush=True)
        remote(["cmake", "-S", root + "/source/surfxr", "-B", root + "/build", "-DCMAKE_BUILD_TYPE=Release"])
        remote(["cmake", "--build", root + "/build", "--parallel", "2"], timeout=args.build_timeout)
        for target in ("surfxr-viewer", "surfxr-dashboard", "surfxr-discover", "surfxr-runtime-bridge"):
            remote(["test", "-x", root + "/build/" + target])
        remote(["test", "-x", root + "/build/surfxr-client"])
        report["native_launcher_build_validated"] = True
        binary_names = ["surfxr-viewer", "surfxr-dashboard", "surfxr-discover", "surfxr-runtime-bridge"]
        binary_names.append("surfxr-client")
        report["binary_sha256"] = json.loads(remote(["python3", "-"], stdin=
            "import hashlib,json,pathlib\nroot=pathlib.Path(" + repr(root + "/build") + ")\n"
            "names=" + repr(binary_names) + "\n"
            "print(json.dumps({n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in names}))\n"))
        report["optional_binary_sha256"] = json.loads(remote(["python3", "-"], stdin=
            "import hashlib,json,pathlib\nroot=pathlib.Path(" + repr(root + "/build") + ")\n"
            "names=[\"driver_00_surfxr_exo_input.so\"]\n"
            "print(json.dumps({n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in names if (root/n).is_file()}))\n"))
        report["viewer_build_validated"] = True
        print("Frame build: running existing offline CTest...", file=sys.stderr, flush=True)
        report["ctest_output"] = remote(["ctest", "--test-dir", root + "/build", "--output-on-failure"], timeout=180)
        report["session_self_test_passed"] = True
        report["viewer_offline_self_test_passed"] = True
        # Detect files changed during the build, before advertising a validated manifest.
        final_hashes = {name: hashlib.sha256((PROJECT.parent / name).read_bytes()).hexdigest() for name in MANIFEST}
        if source_manifest(PROJECT) != MANIFEST or final_hashes != hashes:
            raise RuntimeError("Sources changed during validation; rerun incremental build")
        report["success"] = True
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        report["warning_count"] = sum(len(re.findall(r"warning:", r.get("stderr", ""), re.I)) for r in report["commands"])
        report["completed_utc"] = now()
        output_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(json.dumps({k: report.get(k) for k in ["success", "remote_directory", "warning_count", "error"]}, indent=2))
    if not report["success"]:
        raise SystemExit(1)
    if args.package_release:
        from release_package import package_release
        print(json.dumps(package_release(PROJECT, output_path, args.package_release, args.ssh, args.release_profile), indent=2))


if __name__ == "__main__":
    main()
