"""Adapt existing source-extraction harnesses to modular production files."""
from pathlib import Path
import re


def production_source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def method(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def include_directories(project: Path) -> str:
    # Use the build's authoritative roots. Recursive globbing also finds old
    # isolated build copies, whose stale surfxr/*.hpp can shadow current code.
    manifest = (project / "cmake/public_includes.cmake").read_text(encoding="utf-8")
    names = re.findall(r'"\$\{PROJECT_SOURCE_DIR\}/([^"\r\n]+)"', manifest)
    if not names:
        raise ValueError("public_include_roots_missing")
    roots = [project / name for name in names]
    return " ".join('"' + path.as_posix() + '"' for path in [project, *roots])
