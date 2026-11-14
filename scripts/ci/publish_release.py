"""Publish only a complete, verified set of artifacts from the current workflow."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
from release_version import product_version

ASSETS = ("SurfXR-SteamFrame.zip", "SurfXR-Windows-portable.zip", "SurfXR-Windows-setup.exe")


def verified_assets(directory):
    files = []
    for name in ASSETS:
        asset, checksum = directory / name, directory / (name + ".sha256")
        digest = hashlib.sha256(asset.read_bytes()).hexdigest()
        if checksum.read_text(encoding="utf-8-sig").split()[0] != digest:
            raise ValueError("Release checksum mismatch: " + name)
        files.extend((str(asset), str(checksum)))
    return files


def main():
    version = product_version()
    prerelease = os.environ["RELEASE_PRERELEASE"] == "true"
    if "-" in version.split("+", 1)[0] and not prerelease:
        raise ValueError("A prerelease version requires the pre-release option")
    assets = verified_assets(Path("release"))
    repo, token = os.environ["GITHUB_REPOSITORY"], os.environ["GH_TOKEN"]
    tag = "v" + version
    request = urllib.request.Request(
        os.environ.get("GITHUB_API_URL", "https://api.github.com") + "/repos/" + repo + "/git/ref/tags/" + tag,
        headers={"Authorization": "Bearer " + token, "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            json.load(response)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
    else:
        raise ValueError("Tag already exists; choose a new version: " + tag)
    # gh uploads into a draft before publishing; never overwrite an existing release.
    subprocess.run(["gh", "release", "create", tag, *assets, "--repo", repo,
                    "--target", os.environ["GITHUB_SHA"], "--title", "SurfXR " + version,
                    "--generate-notes", "--prerelease=" + str(prerelease).lower(),
                    "--latest=" + str(not prerelease).lower()], check=True)


if __name__ == "__main__":
    main()
