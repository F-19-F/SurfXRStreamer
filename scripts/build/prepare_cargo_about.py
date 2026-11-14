"""Private xtask helper: obtain the pinned Windows license scanner without Cargo builds."""
import hashlib
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import urllib.request


VERSION = "0.9.3"
ASSET = f"cargo-about-{VERSION}-x86_64-pc-windows-msvc"
URL = f"https://github.com/EmbarkStudios/cargo-about/releases/download/{VERSION}/{ASSET}.tar.gz"
# Official release archive and the executable inside it. Verify cached tools too.
ARCHIVE_SHA256 = "9e2942533ef1c592c4a70bdb384cae1550a3f5fc9ae40379b6662e3d2f16fe02"
EXE_SHA256 = "1d865e2cd72823b466a64831462046e6c9f7b6fb5d2984abfc186ccba1df9d89"


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def prepare(directory):
    directory.mkdir(parents=True, exist_ok=True)
    executable = directory / "cargo-about.exe"
    if executable.is_file() and digest(executable) == EXE_SHA256:
        return executable
    print(f"Downloading verified cargo-about {VERSION} (no Rust compilation)", file=sys.stderr)
    with tempfile.TemporaryDirectory(prefix="download-", dir=directory) as temporary:
        archive = Path(temporary) / "tool.tar.gz"
        with urllib.request.urlopen(URL, timeout=120) as response, archive.open("wb") as output:
            shutil.copyfileobj(response, output)
        if digest(archive) != ARCHIVE_SHA256:
            raise RuntimeError("cargo-about archive SHA-256 mismatch")
        candidate = Path(temporary) / "cargo-about.exe"
        with tarfile.open(archive, "r:gz") as package:
            member = package.getmember(f"{ASSET}/cargo-about.exe")
            if not member.isfile():
                raise RuntimeError("cargo-about executable is not a regular file")
            # Extract only this member's bytes, never archive-supplied filesystem paths.
            with package.extractfile(member) as source, candidate.open("wb") as output:
                shutil.copyfileobj(source, output)
        if digest(candidate) != EXE_SHA256:
            raise RuntimeError("cargo-about executable SHA-256 mismatch")
        candidate.replace(executable)
    return executable


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[2]
    print(prepare(root / "build/tools/cargo-about" / VERSION))
