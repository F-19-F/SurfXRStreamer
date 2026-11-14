"""Check module ownership, 500-line limits and byte-pinned upstream exceptions."""
from pathlib import Path
import hashlib
import json
import os
import sys

PROJECT = Path(__file__).resolve().parents[2] / "surfxr"
CODE_SUFFIXES = {".cpp", ".inl", ".hpp", ".h", ".c", ".rs", ".cs", ".py", ".in", ".ps1", ".cmd", ".js", ".cmake", ".vert", ".frag", ".glsl", ".hlsl", ".sh", ".bat"}
BASELINE = PROJECT / "docs/UPSTREAM_LINE_BASELINE.json"


def sources():
    for root, directories, files in os.walk(PROJECT.parent):
        if Path(root) == PROJECT / "pc/surfxr-server/openvr":
            directories[:] = []
            continue  # External SDK is pinned by the Git submodule, not a source import.
        directories[:] = [d for d in directories if not d.startswith(".") and d not in {"target", "deps", "release", "dist", "__pycache__"}
                           and not (d.startswith("build") and Path(root) in {PROJECT.parent, PROJECT, PROJECT / "pc/surfxr-server"})]
        for name in files:
            path = Path(root) / name
            if path.suffix.lower() in CODE_SUFFIXES or name == "CMakeLists.txt":
                yield path


def main():
    baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
    failures, exceptions, total, maximum = [], 0, 0, 0
    for path in sources():
        data = path.read_bytes()
        count = len(data.splitlines())
        total += 1
        if count <= 500:
            maximum = max(maximum, count)
            continue
        name = path.relative_to(PROJECT.parent).as_posix()
        expected = baseline.get(name)
        if (name.startswith("surfxr/pc/surfxr-server/") and expected
                and expected["lines"] == count
                and expected["sha256"] == hashlib.sha256(data).hexdigest()):
            exceptions += 1
        else:
            failures.append(f"{name}: {count} lines (limit 500; no matching imported baseline)")
    for directory in ("src", "include", "launcher", "tools", "shaders"):
        if any(p.is_file() for p in (PROJECT / directory).rglob("*")):
            failures.append(f"Flat source directory contains files: {directory}")
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        return 1
    print(f"Source structure passed: {total} files; maintained maximum {maximum}; {exceptions} byte-pinned imported exceptions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
