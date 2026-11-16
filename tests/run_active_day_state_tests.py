#!/usr/bin/env python3
"""Compile/run A02 host tests only; never invoke PlatformIO or device tools."""
from __future__ import annotations

import argparse
import json
import hashlib
import os
import signal

from pathlib import Path
import shutil
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", help="Run one group/name from the executable's --list")
    parser.add_argument("--group", choices=["control", "regression", "contract", "model"] +
                        [f"T{i:02d}" for i in range(1, 13)])
    parser.add_argument("--sanitize", nargs="?", const="undefined", choices=["undefined", "address"],
                        help="Host UBSan, or ASan+UBSan when supported by the installed runtime")
    parser.add_argument("--malloc-fence", action="store_true", help="macOS dyld malloc/calloc/realloc fence")
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="Approved scratch directory outside the repository")
    args = parser.parse_args()
    if args.malloc_fence and sys.platform != "darwin":
        parser.error("--malloc-fence is a macOS host check; do not fake the platform")
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which("clang++")
    if not compiler:
        parser.error("clang++ is required; this runner does not install tools")
    json_headers = root / "server/.pio/libdeps/esp32dev/ArduinoJson/src"
    if not (json_headers / "ArduinoJson.h").is_file():
        parser.error("Installed ArduinoJson headers are required; no packages will be installed")
    output_dir = args.build_dir.resolve()
    if output_dir == root or root in output_dir.parents:
        parser.error("Build artifacts must stay outside the repository")
    output_dir.mkdir(parents=True, exist_ok=True)
    binary = output_dir / "test_active_day_state"
    command = [
        compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "tests/host"), "-I", str(json_headers),
        "-include", str(root / "tests/host/json_allocator.h"),
        str(root / "tests/test_active_day_state.cpp"),
        str(root / "server/src/active_day_state.cpp"), "-o", str(binary),
    ]
    if args.sanitize:
        sanitizers = "address,undefined" if args.sanitize == "address" else "undefined"
        command[1:1] = [f"-fsanitize={sanitizers}", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    env = dict(os.environ)
    if args.malloc_fence:
        library = output_dir / "allocation_interpose.dylib"
        subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-dynamiclib",
                        "-undefined", "dynamic_lookup", str(root / "tests/host/allocation_interpose.cpp"),
                        "-o", str(library)], cwd=root, timeout=90, check=True)
        command[1:1] = ["-Wl,-export_dynamic"]
        env["DYLD_INSERT_LIBRARIES"] = str(library)
    print("Host-only compile:", " ".join(command), flush=True)
    built = subprocess.run(command, cwd=root, timeout=90, check=False)
    if built.returncode:
        return built.returncode
    listed = subprocess.run([str(binary), "--list"], capture_output=True, text=True,
                            timeout=10, check=True, env=env).stdout.splitlines()
    if len(listed) != len(set(listed)):
        parser.error("Duplicate registered test identifiers")
    selected = [name for name in listed if (not args.case or name == args.case)
                and (not args.group or name.startswith(args.group + "/"))]
    if not selected:
        parser.error("No matching test cases")
    results = []
    if args.malloc_fence and (not args.group or args.group == "T07") and not args.case:
        for allocator in ["malloc", "calloc", "realloc", "new"]:
            probe = subprocess.run([str(binary), "--probe-allocation", allocator], cwd=root, env=env,
                                   capture_output=True, text=True, timeout=10, check=False)
            name = f"T07/fence_selftest_{allocator}"
            ok = probe.returncode == -signal.SIGABRT
            text = f"{'PASS' if ok else 'FAIL'} {name}: child exit {probe.returncode} (expected SIGABRT)"
            print(text, flush=True)
            results.append({"case": name, "exit_code": 0 if ok else 1, "child_exit_code": probe.returncode,
                            "stdout": text + "\n", "stderr": probe.stderr})
    for name in selected:
        run = subprocess.run([str(binary), name], cwd=root, capture_output=True,
                             text=True, timeout=10, check=False, env=env)
        print(run.stdout.rstrip(), flush=True)
        if run.stderr:
            print(run.stderr, file=sys.stderr, end="")
        results.append({"case": name, "exit_code": run.returncode,
                        "stdout": run.stdout, "stderr": run.stderr})
    summary = {}
    for result in results:
        group = result["case"].split("/", 1)[0]
        counts = summary.setdefault(group, {"passed": 0, "failed": 0})
        counts["passed" if result["exit_code"] == 0 else "failed"] += 1
    inputs = [root / "server/src" / name for name in
              ["schedule.cpp", "schedule.h", "active_day_state.cpp", "active_day_state.h"]]
    inputs += sorted((root / "tests").rglob("*.h"))
    inputs += [root / "tests/test_active_day_state.cpp", Path(__file__).resolve()]
    inputs += [root / "tests/host/allocation_interpose.cpp"]
    all_passed = all(result["exit_code"] == 0 for result in results)
    report = {"summary": summary, "results": results, "sanitizers": args.sanitize,
              "malloc_fence": args.malloc_fence,
              "production_u64_failure_paths": "EXERCISED_ON_HOST" if all_passed and
              {"T05", "T10"} <= summary.keys() else "NOT_FULLY_EXERCISED_IN_THIS_SELECTION",
              "not_exercised": ["ESP32 build/link", "physical Flash/power cuts", "FreeRTOS concurrency",
                                "target allocator/ABI", "physical audio/RTC", "NVS same-value physical rewrite"],
              "input_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in inputs}}
    report_path = output_dir / "results.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("SUMMARY", json.dumps(summary, sort_keys=True))
    print("Production NVS paths:", report["production_u64_failure_paths"])
    print("Report:", report_path)
    return 1 if any(result["exit_code"] != 0 for result in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
