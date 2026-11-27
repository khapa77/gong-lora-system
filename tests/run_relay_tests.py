#!/usr/bin/env python3
"""Compile and run the host-only tests for the relay handler.

Two binaries: the production unit built for the candidate pin (RELAY_PIN=27, the
schematic value) and the shipped "no relay fitted" build (RELAY_PIN=-1). Audio
is a fake backend, GPIO and millis are fakes, NVS/LittleFS are host models.

No Arduino, PlatformIO, device, NVS or LittleFS access is involved.
Build artifacts are written outside the repository.
"""
from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path


def build_and_run(name: str, root: Path, extra_flags: list[str], test: Path,
                  build_dir: Path, sanitize: str | None) -> int:
    source = root / "server" / "src" / "relayhandler.cpp"
    binary = build_dir / name
    arduino_json = root / "server" / ".pio" / "libdeps" / "esp32dev" / "ArduinoJson" / "src"
    if not arduino_json.is_dir():
        print(f"missing ArduinoJson include dir: {arduino_json}", file=sys.stderr)
        return 2

    command = [
        "clang++",
        "-std=c++11",
        "-Wall",
        "-Wextra",
        "-Werror",
        *extra_flags,
        "-I", str(root / "tests" / "host"),
        "-I", str(root / "server" / "src"),
        "-I", str(arduino_json),
        str(source),
        str(test),
        "-o", str(binary),
    ]
    if sanitize:
        command[1:1] = [f"-fsanitize={sanitize}", "-fno-omit-frame-pointer"]

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
    print(f"{name}_sha256={digest}")
    return run.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("/tmp/gong-relay-tests"),
        help="directory for the compiled test binaries (outside the repository)",
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
    build_dir = args.build_dir.resolve()
    if build_dir == root or root in build_dir.parents:
        print("build directory must be outside the repository", file=sys.stderr)
        return 2
    build_dir.mkdir(parents=True, exist_ok=True)

    rc = build_and_run(
        "test_relay",
        root,
        ["-DRELAY_PIN=27"],
        root / "tests" / "test_relay.cpp",
        build_dir,
        args.sanitize,
    )
    if rc != 0:
        return rc
    return build_and_run(
        "test_relay_disabled",
        root,
        ["-DRELAY_PIN=-1"],
        root / "tests" / "test_relay_disabled.cpp",
        build_dir,
        args.sanitize,
    )


if __name__ == "__main__":
    raise SystemExit(main())
