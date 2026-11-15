"""Build-only public OpenXR link reference matching the SteamVR loader name."""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

REVISION = "8899a91c17ce9618f565f42408b47db1d6e9ccc7"  # OpenXR-SDK release-1.1.36
SOURCE_SHA256 = "431d8e0e7b2b5cee68613aafeed686f1ed46ee483a3727d3f905dac1a944b2b6"
LIBRARY = "libopenxr_loader.so"


def prepare(workspace, directory, arguments, jobs):
    cache = workspace / "build/deps/frame-openxr"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / "OpenXR-SDK-1.1.36.tar.gz"
    if not archive.exists():
        url = f"https://codeload.github.com/KhronosGroup/OpenXR-SDK/tar.gz/{REVISION}"
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
            shutil.copyfileobj(response, output)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("openxr_source_hash_mismatch")
    source = cache / ("OpenXR-SDK-" + REVISION)
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(cache, filter="data")
    build = directory / "openxr-link"
    build.mkdir(parents=True, exist_ok=True)
    options = dict(arg[2:].split("=", 1) for arg in arguments if arg.startswith("-D") and "=" in arg)
    flags = [f"-D{name}={options[name]}" for name in ("CMAKE_TOOLCHAIN_FILE", "SURFXR_EXO_SYSROOT") if name in options]
    with (build / "build.log").open("w") as log:
        for command in (["cmake", "-S", workspace / "surfxr/cmake/openxr_link", "-B", build, "-G", "Ninja",
                         "-DCMAKE_BUILD_TYPE=Release", "-DSURFXR_OPENXR_SOURCE=" + str(source), *flags],
                        ["cmake", "--build", build, "--target", "openxr_loader", "--parallel", jobs]):
            subprocess.run([str(v) for v in command], check=True, stdout=log,
                           stderr=subprocess.STDOUT, timeout=600)
    library = build / "lib" / LIBRARY
    dynamic = subprocess.check_output(["readelf", "-d", str(library)], text=True)
    if re.findall(r"\(SONAME\).*?\[(.*?)\]", dynamic) != [LIBRARY]:
        raise ValueError("openxr_link_reference_soname_mismatch")
    return library, source / "include"
