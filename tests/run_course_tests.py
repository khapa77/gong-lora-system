#!/usr/bin/env python3
"""Compile and run the host-only course-model tests.

No Arduino, PlatformIO, device, NVS or LittleFS access is involved.
Build artifacts are written outside the repository.
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
        default=Path("/tmp/gong-course-tests"),
        help="directory for the compiled test binary (outside the repository)",
    )
    parser.add_argument(
        "--sanitize",
        nargs="?",
        const="address,undefined",
        choices=["address", "undefined", "address,undefined"],
        help="optional clang sanitizer set",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    source = root / "server" / "src" / "course_model.cpp"
    header = root / "server" / "src" / "course_model.h"
    test = root / "tests" / "test_course_model.cpp"
    for path in (source, header, test):
        if not path.is_file():
            print(f"missing input: {path}", file=sys.stderr)
            return 2

    build_dir = args.build_dir.resolve()
    if build_dir == root or root in build_dir.parents:
        print("build directory must be outside the repository", file=sys.stderr)
        return 2
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "test_course_model"

    command = [
        "clang++",
        "-std=c++11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(root / "server" / "src"),
        str(source),
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

    print("run:", binary, flush=True)
    run = subprocess.run([str(binary)], cwd=root, text=True, capture_output=True)
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)

    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    print(f"binary_sha256={digest}")
    print(f"build_dir={build_dir}")
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
