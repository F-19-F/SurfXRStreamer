"""PC-side SSH deployment adapter; no second launcher or credential store."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[2] / "surfxr"
sys.path.insert(0, str(PROJECT.parent / "scripts/build"))
from project_manifest import source_manifest


def validated_build(path, host):
    report = json.loads(path.read_text(encoding="utf-8"))
    local = report.get("build_executor") == "local"
    if not (report.get("success") and report.get("viewer_build_validated")
            and report.get("viewer_offline_self_test_passed")
            and report.get("native_launcher_build_validated")
            and (host == "local" if local else report.get("ssh_host") == host)):
        raise ValueError("frame_build_not_validated_for_this_host")
    if local:
        name = report.get("build_directory", "")
        if not re.fullmatch(r"build/frame/[A-Za-z0-9_-]+", name):
            raise ValueError("invalid_local_build_directory")
        directory = (PROJECT.parent / name).resolve()
        if directory.parent != (PROJECT.parent / "build/frame").resolve():
            raise ValueError("invalid_local_build_directory")
        root = str(directory)
    else:
        root = report.get("remote_directory", "")
        if not re.fullmatch(r"/home/[A-Za-z0-9_.-]+/surfxr-build-[A-Za-z0-9_-]+", root):
            raise ValueError("invalid_frame_build_directory")
    current = {n: hashlib.sha256((PROJECT.parent / n).read_bytes()).hexdigest() for n in source_manifest(PROJECT)}
    if report.get("source_sha256") != current:
        raise ValueError("sources_changed_rebuild_before_deployment")
    optional = report.get("optional_binary_sha256", {})
    if optional:
        if not set(optional) <= {"driver_00_surfxr_exo_input.so"}:
            raise ValueError("unexpected_optional_frame_payload")
        for name, expected in optional.items():
            if not re.fullmatch(r"[a-f0-9]{64}", expected):
                raise ValueError("invalid_optional_frame_payload_hash")
            if local:
                if hashlib.sha256((Path(root) / name).read_bytes()).hexdigest() != expected:
                    raise ValueError("optional_frame_payload_changed_rebuild_before_deployment")
                continue
            result = subprocess.run(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", host,
                shlex.join(["sha256sum", root + "/build/" + name])], capture_output=True, text=True, timeout=15)
            if result.returncode or result.stdout.split()[:1] != [expected]:
                raise ValueError("optional_frame_payload_changed_rebuild_before_deployment")
    return root


def remote(host, root, command, request):
    argv = [root + "/build/surfxr-client", "--deploy", command]
    # Configuration uses stdin; do not print or record this request.
    result = subprocess.run(["ssh", "-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
        "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
        host, shlex.join(argv)], input=json.dumps(request), capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=180)
    try:
        response = json.loads(result.stdout)
    except ValueError:
        raise ValueError("frame_ssh_command_failed_check_access") from None
    if result.returncode or not response.get("ok"):
        reason = response.get("error", "frame_remote_stage_failed")
        if not isinstance(reason, str) or not re.fullmatch(r"[a-z_]{1,128}", reason):
            reason = "frame_remote_stage_failed"
        raise ValueError(reason)
    return response


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("inspect", "stop", "deploy", "start"))
    parser.add_argument("--ssh", default="steamos@frame")
    parser.add_argument("--build-report", type=Path)
    parser.add_argument("--mode", choices=("desktop", "vr"), default="desktop")
    parser.add_argument("--link", choices=("lan", "direct"), default="direct")
    parser.add_argument("--pc-lan", default="", help="Empty selects Frame-side SURFXR LAN discovery")
    parser.add_argument("--pc-direct", default="", help="Empty selects Frame-side SURFXR 6 GHz discovery")
    parser.add_argument("--frame-lan", default="")
    parser.add_argument("--frame-direct", default="")
    args = parser.parse_args()
    try:
        if args.ssh.startswith("-") or not re.fullmatch(r"[A-Za-z0-9_.-]+@[A-Za-z0-9_.:-]+", args.ssh):
            raise ValueError("invalid_ssh_host")
        request = {name: getattr(args, name) for name in
                   ("mode", "link", "pc_lan", "pc_direct", "frame_lan", "frame_direct")}
        if not args.build_report:
            raise ValueError("validated_build_report_required")
        root = validated_build(args.build_report, args.ssh)
        response = remote(args.ssh, root, args.command, request)
        request.clear()
        print(json.dumps(response))
        return 0
    except Exception as error:
        reason = str(error) if isinstance(error, ValueError) and re.fullmatch(r"[a-z_]{1,128}", str(error)) else "one_click_stage_failed"
        print(json.dumps({"ok": False, "error": reason}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
