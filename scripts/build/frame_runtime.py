"""Private media dependencies for build_viewer --local ARM64 releases."""
import hashlib
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

SOURCES = {
    "ffmpeg-7.1.1": ("https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz",
                     "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"),
    "dav1d-1.5.1": ("https://downloads.videolan.org/pub/videolan/dav1d/1.5.1/dav1d-1.5.1.tar.xz",
                    "401813f1f89fa8fd4295805aa5284d9aed9bc7fc1fdbe554af4292f64cbabe21"),
}


def prepare(workspace, directory, arguments, jobs):
    options = dict(arg[2:].split("=", 1) for arg in arguments if arg.startswith("-D") and "=" in arg)
    sdk = options.get("SURFXR_EXO_SYSROOT")
    native = options.get("SURFXR_BUNDLE_MEDIA", "OFF").upper() in {"ON", "TRUE", "1"} and not options.get("CMAKE_TOOLCHAIN_FILE")
    if not native and (not sdk or not options.get("CMAKE_TOOLCHAIN_FILE", "").endswith("linux-arm64.cmake")):
        return None
    sdk = Path(sdk).resolve() if not native else None
    cache = workspace / "build/deps/frame-ffmpeg"
    cache.mkdir(parents=True, exist_ok=True)
    dep = directory / "media-dependencies"
    dep.mkdir(exist_ok=True)
    prefix = dep / "installed"
    toolchain = (platform.machine() + repr(platform.libc_ver()) + subprocess.check_output(["cc", "--version"], text=True)).encode() if native else (sdk / "usr/lib/libc.so.6").read_bytes()
    signature = hashlib.sha256(Path(__file__).read_bytes() + str(sdk).encode() + toolchain).hexdigest()
    stamp = dep / "validated-input.json"

    def run(argv, cwd=dep, env=None):
        subprocess.run([str(v) for v in argv], cwd=cwd, env=env, check=True,
                       stdout=log, stderr=subprocess.STDOUT, timeout=1200)

    rebuild = not stamp.exists() or stamp.read_text() != signature
    with (dep / "build.log").open("w" if rebuild else "a") as log:
        archives = {}
        for name, (url, digest) in SOURCES.items():
            archive = cache / (name + ".tar.xz")
            if not archive.exists():
                with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
                    shutil.copyfileobj(response, output)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
                raise ValueError("media_dependency_source_hash_mismatch")
            archives[name] = archive
            if not (dep / name).exists():
                with tarfile.open(archive) as source:
                    source.extractall(dep, filter="data")
        if rebuild:
            # Only private generated dependency directories are replaced.
            for child in (prefix, dep / "dav1d-build", dep / "ffmpeg-build"):
                if child.exists():
                    shutil.rmtree(child)
            cross = dep / "cross.ini"
            if not native:
                cross.write_text("[binaries]\nc = 'aarch64-linux-gnu-gcc'\nar = 'aarch64-linux-gnu-ar'\n"
                             "strip = 'aarch64-linux-gnu-strip'\n[host_machine]\nsystem = 'linux'\n"
                             "cpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n"
                             "[built-in options]\nc_args = ['--sysroot=" + str(sdk) + "']\n"
                             "c_link_args = ['--sysroot=" + str(sdk) + "', '-B" + str(sdk / "usr/lib") + "/']\n")
            meson_flags = [] if native else ["--cross-file", cross]
            run(["meson", "setup", "dav1d-build", "dav1d-1.5.1", *meson_flags,
                 "--prefix", prefix, "--libdir", "lib", "--buildtype", "release",
                 "-Ddefault_library=static", "-Denable_tools=false", "-Denable_tests=false"])
            run(["ninja", "-C", "dav1d-build", "-j", jobs, "install"])
            ffbuild = dep / "ffmpeg-build"
            ffbuild.mkdir()
            env = dict(os.environ, PKG_CONFIG_PATH=str(prefix / "lib/pkgconfig"),
                       PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"), PKG_CONFIG_SYSROOT_DIR="")
            target_flags = ["--arch=" + platform.machine(), "--target-os=linux"] if native else [
                 "--arch=aarch64", "--target-os=linux", "--enable-cross-compile",
                 "--cross-prefix=aarch64-linux-gnu-", "--sysroot=" + str(sdk),
                 "--extra-cflags=-B" + str(sdk / "usr/lib") + "/",
                 "--extra-ldflags=-B" + str(sdk / "usr/lib") + "/"]
            run([dep / "ffmpeg-7.1.1/configure", "--prefix=" + str(prefix), *target_flags,
                 "--disable-autodetect", "--disable-everything", "--enable-shared", "--disable-static",
                 "--disable-programs", "--disable-doc", "--disable-debug", "--disable-network",
                 "--disable-avdevice", "--disable-avfilter", "--disable-postproc", "--disable-swresample",
                 "--enable-libdav1d", "--enable-v4l2-m2m", "--enable-decoder=h264,hevc,vp8,vp9,libdav1d,h264_v4l2m2m,hevc_v4l2m2m",
                 "--enable-parser=h264,hevc,vp8,vp9,av1", "--enable-demuxer=h264,hevc,av1,obu",
                 "--enable-protocol=file", "--enable-swscale", "--enable-pic"], ffbuild, env)
            components = (ffbuild / "config_components.h").read_text()
            for decoder in ("H264", "HEVC", "VP8", "VP9", "LIBDAV1D", "H264_V4L2M2M", "HEVC_V4L2M2M"):
                if "#define CONFIG_" + decoder + "_DECODER 1" not in components:
                    raise ValueError("private_media_decoder_missing_" + decoder.lower())
            run(["make", "-j", jobs], ffbuild, env)
            run(["make", "install"], ffbuild, env)
            stamp.write_text(signature)
        runtime = directory / "runtime"
        libs = runtime / "lib"
        licenses = runtime / "licenses"
        libs.mkdir(parents=True, exist_ok=True)
        licenses.mkdir(exist_ok=True)
        for path in (prefix / "lib").glob("*.so.*"):
            if re.fullmatch(r"lib(avcodec|avutil|avformat|swscale)\.so\.[0-9]+", path.name):
                shutil.copyfile(path, libs / path.name)
        for name, archive in archives.items():
            shutil.copyfile(archive, licenses / archive.name)
        shutil.copyfile(dep / "ffmpeg-7.1.1/COPYING.LGPLv2.1", licenses / "FFmpeg.LICENSE")
        shutil.copyfile(dep / "dav1d-1.5.1/COPYING", licenses / "dav1d.LICENSE")
        shutil.copyfile(Path(__file__), licenses / "frame_runtime.py")
        shutil.copyfile(dep / "ffmpeg-build/config.h", licenses / "FFmpeg.config.h")
        (licenses / "README.txt").write_text(
            "FFmpeg is dynamically linked under LGPL-2.1-or-later; dav1d is statically linked into it under BSD-2-Clause.\n"
            "Unmodified corresponding source archives, configuration and the build recipe are included.\n"
            "Users may replace compatible libraries in the adjacent lib directory and relink modified sources.\n")
    allowed = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "libgcc_s.so.1", "ld-linux-aarch64.so.1"}
    bundled = {p.name for p in libs.iterdir()}
    if any(not any(name.startswith("lib" + component + ".so.") for name in bundled)
           for component in ("avcodec", "avutil", "avformat", "swscale")):
        raise ValueError("private_media_runtime_incomplete")
    for library in libs.iterdir():
        dynamic = subprocess.check_output(["readelf" if native else "aarch64-linux-gnu-readelf", "-d", str(library)], text=True)
        needed = set(re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic))
        if needed - bundled - allowed:
            raise ValueError("private_media_runtime_has_unbundled_dependencies")
    return prefix


def runtime_hashes(directory):
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (directory / "runtime").rglob("*") if p.is_file()}
