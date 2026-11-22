#!/usr/bin/env python3
"""Compile and run host-only emergency/reboot tests.

schedule.cpp is compiled as a translation unit through the test file; LittleFS,
NVS, time and audio are host fakes. No device is touched.
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
        default=Path("/tmp/gong-emergency-tests"),
        help="directory for the compiled test binary",
    )
    parser.add_argument(
        "--sanitize",
        nargs="?",
        const="undefined",
        choices=["undefined", "address", "address,undefined"],
        help="build with clang sanitizers",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    sources = [
        root / "server" / "src" / "active_day_state.cpp",
        root / "server" / "src" / "course_model.cpp",
        root / "server" / "src" / "course_store.cpp",
        root / "server" / "src" / "emergency.cpp",
    ]
    test = root / "tests" / "test_emergency.cpp"
    arduino_json = root / "server" / ".pio" / "libdeps" / "esp32dev" / "ArduinoJson" / "src"
    for path in [*sources, test]:
        if not path.is_file():
            print(f"missing input: {path}", file=sys.stderr)
            return 2
    if not arduino_json.is_dir():
        print(f"missing ArduinoJson headers: {arduino_json}", file=sys.stderr)
        return 2

    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "test_emergency"

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
        *[str(p) for p in sources],
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

    ran = subprocess.run([str(binary)], cwd=root, text=True, capture_output=True)
    sys.stdout.write(ran.stdout)
    sys.stderr.write(ran.stderr)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    print(f"binary_sha256={digest}")
    print(f"build_dir={build_dir}")
    return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
