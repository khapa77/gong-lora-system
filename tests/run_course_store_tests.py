#!/usr/bin/env python3
"""Compile and run host-only course_store tests.

LittleFS and NVS are emulated under tests/host. No device, flashing or real
NVS/LittleFS access is involved. Build artifacts are written outside the repo.
"""
from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("/tmp/gong-course-store-tests"),
        help="directory for the compiled test binary (outside the repository)",
    )
    parser.add_argument(
        "--sanitize",
        nargs="?",
        const="address,undefined",
        choices=["address,undefined"],
        help="optional clang sanitizer set",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    sources = [
        root / "server" / "src" / "course_model.cpp",
        root / "server" / "src" / "course_store.cpp",
    ]
    headers = [
        root / "server" / "src" / "course_model.h",
        root / "server" / "src" / "course_store.h",
    ]
    test = root / "tests" / "test_course_store.cpp"
    arduino_json = root / "server" / ".pio" / "libdeps" / "esp32dev" / "ArduinoJson" / "src"
    for path in [*sources, *headers, test]:
        if not path.is_file():
            print(f"missing input: {path}", file=sys.stderr)
            return 2
    if not arduino_json.is_dir():
        print(f"missing ArduinoJson include dir: {arduino_json}", file=sys.stderr)
        return 2

    build_dir = args.build_dir.resolve()
    if build_dir == root or root in build_dir.parents:
        print("build directory must be outside the repository", file=sys.stderr)
        return 2
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "test_course_store"

    command = [
        "clang++",
        "-std=c++11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(root / "tests" / "host"),
        "-I",
        str(root / "server" / "src"),
        "-I",
        str(arduino_json),
        *[str(path) for path in sources],
        str(test),
        "-o",
        str(binary),
    ]
    if args.sanitize:
        command[1:1] = [f"-fsanitize={args.sanitize}", "-fno-omit-frame-pointer"]

    print("compile:", " ".join(command), flush=True)
    compiled = subprocess.run(command, cwd=root, text=True, capture_output=True)
    if compiled.returncode != 0:
        sys.stdout.write(compiled.stdout)
        sys.stderr.write(compiled.stderr)
        return compiled.returncode

    run = subprocess.run([str(binary)], cwd=root, text=True, capture_output=True)
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    print(f"binary_sha256={digest}")
    print(f"build_dir={build_dir}")
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
