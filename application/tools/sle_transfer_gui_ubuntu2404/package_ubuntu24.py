from __future__ import annotations

import gzip
import hashlib
import io
import os
import tarfile
from pathlib import Path


PACKAGE = "sle-5hop-transfer-stable260"
VERSION = "1.0.0-1"
APP_DIR = Path(__file__).resolve().parent
RELEASE_DIR = APP_DIR.parent / "release"
SOURCE_DIR_NAME = "NearLink-SLE-5Hop-Transfer-Stable260-Ubuntu24.04"

APP_FILES = (
    "app.py",
    "transfer.py",
    "protocol.py",
    "stable260_app.py",
)
SOURCE_FILES = APP_FILES + (
    "requirements.txt",
    "start_ubuntu24.sh",
    "check_ubuntu24.sh",
    "build_ubuntu24.sh",
    "SLE-5Hop-Transfer-Stable260-Ubuntu24.04.spec",
    "sle-5hop-transfer-stable260.desktop",
    "README_Ubuntu24.04.md",
    "test_app.py",
    "test_transfer.py",
    "test_protocol.py",
)

LAUNCHER = b"""#!/usr/bin/env bash
set -euo pipefail
exec python3 /opt/sle-5hop-transfer-stable260/stable260_app.py \"$@\"
"""

CONTROL = f"""Package: {PACKAGE}
Version: {VERSION}
Section: utils
Priority: optional
Architecture: all
Depends: python3 (>= 3.10), python3-tk, python3-serial, xdg-utils, fonts-noto-cjk
Maintainer: NearLink SLE Project
Description: NearLink SLE five-hop Stable260 file transfer GUI
 Native Ubuntu GUI for five-hop link setup, closed-loop 260 kbps pacing,
 reliable file transfer, path diagnostics, and SHA-256 verification.
""".encode("utf-8")


def add_bytes(tar: tarfile.TarFile, name: str, data: bytes, mode: int) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.mtime = 0
    tar.addfile(info, io.BytesIO(data))


def add_directory(tar: tarfile.TarFile, name: str) -> None:
    info = tarfile.TarInfo(name.rstrip("/") + "/")
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.uid = 0
    info.gid = 0
    info.mtime = 0
    tar.addfile(info)


def make_tar_gz(path: Path, entries: list[tuple[str, bytes, int]]) -> None:
    with path.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w", format=tarfile.GNU_FORMAT) as tar:
                directories: set[str] = set()
                for name, _, _ in entries:
                    parent = Path(name).parent
                    while str(parent) not in (".", ""):
                        directories.add(parent.as_posix())
                        parent = parent.parent
                for directory in sorted(directories, key=lambda item: (item.count("/"), item)):
                    add_directory(tar, directory)
                for name, data, mode in entries:
                    add_bytes(tar, name, data, mode)


def write_ar(path: Path, members: list[tuple[str, bytes]]) -> None:
    with path.open("wb") as archive:
        archive.write(b"!<arch>\n")
        for name, data in members:
            encoded_name = (name + "/").encode("ascii")
            header = (
                encoded_name.ljust(16)
                + b"0".ljust(12)
                + b"0".ljust(6)
                + b"0".ljust(6)
                + b"100644".ljust(8)
                + str(len(data)).encode("ascii").ljust(10)
                + b"`\n"
            )
            archive.write(header)
            archive.write(data)
            if len(data) % 2:
                archive.write(b"\n")


def build_deb() -> Path:
    desktop = (APP_DIR / "sle-5hop-transfer-stable260.desktop").read_bytes()
    data_entries: list[tuple[str, bytes, int]] = []
    for filename in APP_FILES:
        data_entries.append(
            (f"./opt/{PACKAGE}/{filename}", (APP_DIR / filename).read_bytes(), 0o644)
        )
    data_entries.extend(
        (
            (f"./usr/bin/{PACKAGE}", LAUNCHER, 0o755),
            (
                f"./usr/share/applications/{PACKAGE}.desktop",
                desktop,
                0o644,
            ),
        )
    )

    md5_lines = []
    for name, data, _ in data_entries:
        md5_lines.append(f"{hashlib.md5(data).hexdigest()}  {name.removeprefix('./')}")
    control_entries = [
        ("./control", CONTROL, 0o644),
        ("./md5sums", ("\n".join(md5_lines) + "\n").encode("utf-8"), 0o644),
    ]

    scratch = RELEASE_DIR / ".ubuntu24-package"
    scratch.mkdir(parents=True, exist_ok=True)
    control_path = scratch / "control.tar.gz"
    data_path = scratch / "data.tar.gz"
    make_tar_gz(control_path, control_entries)
    make_tar_gz(data_path, data_entries)

    output = RELEASE_DIR / f"{PACKAGE}_{VERSION}_all.deb"
    write_ar(
        output,
        [
            ("debian-binary", b"2.0\n"),
            ("control.tar.gz", control_path.read_bytes()),
            ("data.tar.gz", data_path.read_bytes()),
        ],
    )
    control_path.unlink()
    data_path.unlink()
    scratch.rmdir()
    return output


def build_source_archive() -> Path:
    entries = []
    for filename in SOURCE_FILES:
        mode = 0o755 if filename.endswith(".sh") else 0o644
        entries.append(
            (
                f"{SOURCE_DIR_NAME}/{filename}",
                (APP_DIR / filename).read_bytes(),
                mode,
            )
        )
    output = RELEASE_DIR / f"{SOURCE_DIR_NAME}.tar.gz"
    make_tar_gz(output, entries)
    return output


def main() -> None:
    RELEASE_DIR.mkdir(parents=True, exist_ok=True)
    deb = build_deb()
    source = build_source_archive()
    print(deb)
    print(source)


if __name__ == "__main__":
    main()
