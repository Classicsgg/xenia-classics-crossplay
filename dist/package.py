"""Build the player download: Xenia-Classics-Crossplay-b<N>.zip.

    python dist/package.py            (after `python xenia-build.py build --config=release`)

The zip holds the release exe plus everything in dist/ except this script: the config
template (crossplay already set up), keyboard/mouse bindings, portable.txt, the README,
LICENSE, CoD4's Default.mpdata and MW2's xstorage playlists. <N> is bridge::kCrossplayBuild
from src/xenia/kernel/xbridge.h. Prints the size and SHA-256 for the release notes and the
ranked site's download settings (Xenia URL / version / size / SHA-256).
"""
import hashlib
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
EXE = os.path.join(ROOT, "build", "bin", "Windows", "Release", "xenia_canary_netplay.exe")
TOP = "Xenia-Classics-Crossplay"


def main():
    with open(os.path.join(ROOT, "src", "xenia", "kernel", "xbridge.h"), encoding="utf-8") as f:
        build = int(re.search(r"kCrossplayBuild\s*=\s*(\d+)", f.read()).group(1))
    if not os.path.exists(EXE):
        sys.exit("no release exe: run python xenia-build.py build --config=release first")
    out = os.path.join(ROOT, "build", f"{TOP}-b{build}.zip")
    files = [(EXE, "xenia_canary_netplay.exe"), (os.path.join(ROOT, "LICENSE"), "LICENSE")]
    for base, _, names in os.walk(DIST):
        for n in names:
            p = os.path.join(base, n)
            if p != os.path.abspath(__file__):
                files.append((p, os.path.relpath(p, DIST)))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for src, rel in sorted(files, key=lambda f: f[1]):
            z.write(src, f"{TOP}/{rel.replace(os.sep, '/')}")
    data = open(out, "rb").read()
    print(f"{out}\nbuild {build}  size {len(data)} bytes  sha256 {hashlib.sha256(data).hexdigest()}")
    for src, rel in sorted(files, key=lambda f: f[1]):
        print(f"  {os.path.getsize(src):>10}  {rel}")


if __name__ == "__main__":
    main()
