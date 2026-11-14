"""Repository-relative build input manifest, shared by local/SSH build and release."""
from pathlib import Path
import os

SOURCE_DIRECTORIES = ("shared", "frame", "pc", "tests", "cmake")
SOURCE_SUFFIXES = {".otf", ".cpp", ".hpp", ".h", ".cmake", ".in", ".py", ".ps1", ".vert", ".frag", ".glsl", ".LICENSE", ".service", ".js"}


def source_manifest(project: Path) -> list[str]:
    repository = project.parent
    names = {"surfxr/CMakeLists.txt", "surfxr/README.md"}
    for directory in SOURCE_DIRECTORIES:
        for root, directories, files in os.walk(project / directory):
            directories[:] = [d for d in directories if d not in {"surfxr-server", "__pycache__"}]
            for name in files:
                path = Path(root) / name
                if path.suffix in SOURCE_SUFFIXES or path.name in {"CMakeLists.txt", "README.md"}:
                    names.add(path.relative_to(repository).as_posix())
    sdk = "surfxr/pc/surfxr-server/openvr"
    names.update(f"{sdk}/headers/{name}" for name in ("openvr.h", "openvr_driver.h"))
    names.update(f"{sdk}/lib/{arch}/libopenvr_api.so" for arch in ("linux64", "linuxarm64"))
    if any(not (repository / name).is_file() for name in names if name.startswith(sdk + "/")):
        raise FileNotFoundError("OpenVR SDK is missing. Run git submodule update --init --recursive from the repository root.")
    # Build and shader tools move with the source snapshot, without maintenance-only
    # export utilities or release/launch programs unrelated to compilation.
    for directory in ("shaders", "fonts", "transport", "tests"):
        names.update(path.relative_to(repository).as_posix()
                     for path in (repository / "scripts" / directory).rglob("*.py"))
    for name in ("build_viewer.py", "build_local.py", "project_manifest.py", "frame_runtime.py", "release_version.py"):
        names.add("scripts/build/" + name)
    return sorted(names)
