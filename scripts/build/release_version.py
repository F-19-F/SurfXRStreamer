"""Product release metadata; the independently fixed wire version is untouched."""
import argparse
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SEMVER = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?")


def validate(value):
    match = SEMVER.fullmatch(value)
    if not match or len(value) > 64:
        raise ValueError("Version must be SemVer, for example 1.2.0 or 1.2.0-rc.1 (without v)")
    if match[4] and any(part.isdigit() and len(part) > 1 and part.startswith("0") for part in match[4].split(".")):
        raise ValueError("Numeric prerelease identifiers must not have leading zeroes")
    return value


def product_version():
    requested = os.environ.get("SURFXR_RELEASE_VERSION", "")
    if not requested:
        manifest = (ROOT / "surfxr/pc/surfxr-server/Cargo.toml").read_text(encoding="utf-8")
        requested = re.search(r'^version = "([^"]+)"', manifest, re.M)[1]
    return validate(requested)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    version = product_version()
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as output:
            output.write("version=" + version + "\ntag=v" + version + "\n")
    print(version)
