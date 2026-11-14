"""Pinned portable PC dependencies for the existing release packager.

Vendor archives stay in the ignored build cache. Never installs tools/services,
changes PATH, or copies the packager's Python / SSH configuration into a release.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import urllib.request
import zipfile

PACKAGES = (
    {"name":"openssh", "version":"10.0.0.0p2-Preview", "prefix":"OpenSSH-Win64/",
     "url":"https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-Win64.zip",
     "sha256":"23f50f3458c4c5d0b12217c6a5ddfde0137210a30fa870e98b29827f7b43aba5",
     "required":("ssh.exe","scp.exe","sftp.exe","ssh-keygen.exe","libcrypto.dll","LICENSE.txt","NOTICE.txt")},
    {"name":"python", "version":"3.14.7", "prefix":"",
     "url":"https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip",
     "sha256":"d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15",
     "required":("python.exe","python314.dll","python314.zip","python314._pth","_ctypes.pyd","LICENSE.txt")},
)
SSH_CLIENT_FILES = {"ssh.exe","scp.exe","sftp.exe","ssh-keygen.exe","ssh-keyscan.exe",
                    "ssh-pkcs11-helper.exe","ssh-sk-helper.exe","ssh-shellhost.exe"}


def vendor_archive(package: dict, cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / (package["name"] + "-" + package["version"] + ".zip")
    if not target.exists():
        temporary = target.with_suffix(".download")
        try:
            request = urllib.request.Request(package["url"], headers={"User-Agent":"SteamFrameClient-release-builder"})
            with urllib.request.urlopen(request, timeout=30) as source, temporary.open("wb") as output:
                total = 0
                while chunk := source.read(1024 * 1024):
                    total += len(chunk)
                    if total > 128 * 1024 * 1024:
                        raise ValueError("portable_dependency_download_too_large")
                    output.write(chunk)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != package["sha256"]:
                raise ValueError("portable_dependency_archive_hash_mismatch")
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(target.read_bytes()).hexdigest() != package["sha256"]:
        raise ValueError("portable_dependency_archive_hash_mismatch")
    return target


def extract_vendor(archive: Path, target: Path, package: dict) -> None:
    target.mkdir(parents=True, exist_ok=True)
    names = set()
    with zipfile.ZipFile(archive) as source:
        if sum(entry.file_size for entry in source.infolist()) > 256 * 1024 * 1024:
            raise ValueError("portable_dependency_archive_too_large")
        for entry in source.infolist():
            path = PurePosixPath(entry.filename)
            if (path.is_absolute() or ".." in path.parts or ":" in entry.filename or "\\" in entry.filename
                    or stat.S_ISLNK(entry.external_attr >> 16)):
                raise ValueError("invalid_portable_dependency_path")
            if entry.is_dir():
                continue
            if not entry.filename.startswith(package["prefix"]):
                raise ValueError("unexpected_portable_dependency_root")
            relative = entry.filename[len(package["prefix"]):]
            # Bundle clients, DLL dependencies and upstream notices; no sshd,
            # agent service or third-party system installation scripts.
            if package["name"] == "openssh" and not (relative in SSH_CLIENT_FILES or
                    "/" not in relative and relative.lower().endswith((".dll",".txt"))):
                continue
            if not relative or relative.lower() in names:
                raise ValueError("duplicate_portable_dependency_path")
            names.add(relative.lower())
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            with source.open(entry) as incoming, destination.open("wb") as outgoing:
                shutil.copyfileobj(incoming, outgoing)
    for name in package["required"]:
        if not (target / name).is_file():
            raise ValueError("portable_dependency_required_file_missing")


def check_pc_tools(root: Path) -> bool:
    if os.name != "nt":
        return False
    ssh = subprocess.run([str(root/"openssh/ssh.exe"), "-V"], capture_output=True, timeout=15)
    if ssh.returncode or b"OpenSSH" not in ssh.stderr + ssh.stdout:
        raise ValueError("bundled_ssh_offline_check_failed")
    python = subprocess.run([str(root/"python/python.exe"), "-I", "-c",
        "import ctypes,json,ssl,zipfile; from ctypes import wintypes; "
        "ctypes.WinDLL('crypt32'); ctypes.WinDLL('kernel32'); print('portable-python-ok')"],
        capture_output=True, timeout=15)
    if python.returncode or b"portable-python-ok" not in python.stdout:
        raise ValueError("bundled_python_offline_check_failed")
    return True


def bundle_pc_tools(folder: Path, cache: Path) -> dict:
    root = folder / "tools/windows-x64"
    sources = {}
    for package in PACKAGES:
        extract_vendor(vendor_archive(package,cache),root/package["name"],package)
        sources[package["name"]] = {key:package[key] for key in ("version","url","sha256")}
        sources[package["name"]]["license"] = "tools/windows-x64/" + package["name"] + "/LICENSE.txt"
    checked = check_pc_tools(root)
    metadata = {"platform":"windows-x64", "sources":sources, "offline_checked":checked,
                "ssh":"tools/windows-x64/openssh/ssh.exe", "scp":"tools/windows-x64/openssh/scp.exe",
                "python":"tools/windows-x64/python/python.exe"}
    (folder/"THIRD_PARTY_TOOLS.json").write_text(json.dumps(metadata,indent=2)+"\n",encoding="utf-8")
    return metadata
