"""Local Linux/WSL execution for build_viewer; uses the same manifest and CTest."""
from pathlib import Path
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
from project_manifest import source_manifest

BINARIES = ("surfxr-viewer", "surfxr-dashboard", "surfxr-discover",
            "surfxr-client", "surfxr-runtime-bridge")
DRIVER = "driver_00_surfxr_exo_input.so"


def build(project, args):
    if os.name != "posix":
        raise ValueError("--local requires Linux, including WSL")
    build_root = (project.parent / "build/frame").resolve()
    directory = (args.build_directory or build_root / "local").resolve()
    if directory.parent != build_root or not re.fullmatch(r"[A-Za-z0-9_-]+", directory.name):
        raise ValueError("Local build directory must be a named child of repository build/frame")
    directory.mkdir(parents=True, exist_ok=True)
    output = (args.output or directory / "validation.json").resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    hashes = {n: hashlib.sha256((project.parent / n).read_bytes()).hexdigest()
              for n in source_manifest(project)}
    report = {"started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "build_executor": "local", "build_directory": directory.relative_to(project.parent.resolve()).as_posix(),
              "source_project": str(project), "source_sha256": hashes,
              "standalone": True, "commands": [], "success": False,
              "scope": "Linux/WSL build and CPU/isolated loopback CTest only",
              "openxr_session_started": False, "live_discovery_or_session_started": False}

    def run(argv, timeout=120):
        result = subprocess.run([str(v) for v in argv], capture_output=True, text=True,
                                errors="replace", timeout=timeout)
        report["commands"].append({"argv": [str(v) for v in argv], "returncode": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            print(result.stdout[-12000:] + result.stderr[-12000:], flush=True)
            raise RuntimeError(f"Command failed ({result.returncode}): {argv[0]}")
        return result.stdout

    try:
        from frame_runtime import prepare, runtime_hashes
        media = prepare(project.parent, directory, args.cmake_argument, args.jobs)
        cmake_arguments = list(args.cmake_argument)
        if media:
            cmake_arguments.append("-DSURFXR_FFMPEG_ROOT=" + str(media))
            if not any(arg.startswith("-DSURFXR_EXO_SYSROOT=") for arg in args.cmake_argument):
                from frame_xr_link import prepare as prepare_xr
                loader, headers = prepare_xr(project.parent, directory, args.cmake_argument, args.jobs)
                cmake_arguments += ["-DFRAME_OPENXR_LOADER=" + str(loader), "-DOPENXR_INCLUDE_DIR=" + str(headers)]
        for script in ("compile_foveation_shaders.py", "compile_desktop_shaders.py",
                       "compile_image_enhancement_shaders.py"):
            run(["python3", project.parent / "scripts/shaders" / script, "--check"])
        run(["python3", project.parent / "scripts/shaders/validate_foveation_gamma2.py"])
        print("SurfXR: configuring local Linux build", flush=True)
        run(["cmake", "-S", project, "-B", directory, "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", *cmake_arguments])
        print("SurfXR: compiling locally", flush=True)
        run(["cmake", "--build", directory, "--parallel", args.jobs], args.build_timeout)
        report["runtime_sha256"] = runtime_hashes(directory)
        if media:
            dependency_log = directory / "media-dependencies/build.log"
            report["media_dependency_warning_count"] = len(re.findall(r"warning:", dependency_log.read_text(errors="replace"), re.I))
            report["media_dependency_log"] = dependency_log.relative_to(project.parent).as_posix()
        report["binary_sha256"] = {}
        for name in BINARIES:
            data = (directory / name).read_bytes()
            machine = int.from_bytes(data[18:20], "little")
            architecture = {183: "aarch64", 62: "x86_64"}.get(machine)
            if data[:6] != b"\x7fELF\x02\x01" or not architecture:
                raise RuntimeError("Unsupported local executable architecture")
            if report.get("architecture", architecture) != architecture:
                raise RuntimeError("Mixed executable architectures")
            report["architecture"] = architecture
            report["binary_sha256"][name] = hashlib.sha256(data).hexdigest()
            if media:
                dynamic = run(["readelf", "-d", directory / name])
                needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
                if any(lib.startswith("libopenxr_loader.so.") for lib in needed):
                    raise RuntimeError("Release must link SteamVR's unversioned OpenXR loader")
        report["optional_binary_sha256"] = ({DRIVER: hashlib.sha256((directory / DRIVER).read_bytes()).hexdigest()}
                                             if (directory / DRIVER).is_file() else {})
        report["viewer_build_validated"] = report["native_launcher_build_validated"] = True
        print("SurfXR: running CPU and isolated loopback CTest", flush=True)
        report["ctest_output"] = run(["ctest", "--test-dir", directory, "--output-on-failure"], 600)
        report["viewer_offline_self_test_passed"] = True
        report["session_self_test_passed"] = True
        current = {n: hashlib.sha256((project.parent / n).read_bytes()).hexdigest()
                   for n in source_manifest(project)}
        if current != hashes:
            raise RuntimeError("Sources changed during validation; rerun build")
        report["success"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        report["warning_count"] = sum(len(re.findall(r"warning:", c.get("stdout", "") + c.get("stderr", ""), re.I))
                                      for c in report["commands"])
        report["completed_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: report.get(k) for k in ("success", "architecture", "warning_count", "error")}), flush=True)
    if not report["success"]:
        raise SystemExit(1)
    return output
