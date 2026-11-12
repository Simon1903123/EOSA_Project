#!/usr/bin/env python3
"""Run the common scratch-os suites against a kernel and filesystem image."""

import argparse
import difflib
import json
import os
import re
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time


PROMPT = b"scratch> "
IMAGE_ADDRESS = "0x00200000"


class TestFailure(Exception):
    pass


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("small", "big"), required=True)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--startup-timeout", type=float, default=5.0)
    parser.add_argument("--test-timeout", type=float, default=5.0)
    parser.add_argument("--global-timeout", type=float, default=90.0)
    parser.add_argument("--test", action="append", dest="selected_tests",
                        help="run only this named test (repeatable)")
    parser.add_argument("--list", action="store_true", help="list configured tests and exit")
    return parser.parse_args()


def load_suite(root, name, selected):
    path = root / "suites" / f"{name}.json"
    try:
        suite = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TestFailure(f"cannot load suite {path}: {error}") from error
    if suite.get("version") != 1 or not isinstance(suite.get("tests"), list):
        raise TestFailure(f"invalid suite manifest: {path}")
    tests = suite["tests"]
    names = [test.get("name") for test in tests]
    if any(not isinstance(name, str) or not name for name in names) or len(set(names)) != len(names):
        raise TestFailure(f"suite has invalid or duplicate test names: {path}")
    if selected:
        unknown = sorted(set(selected) - set(names))
        if unknown:
            raise TestFailure("unknown test(s): " + ", ".join(unknown))
        wanted = set(selected)
        tests = [test for test in tests if test["name"] in wanted]
    return suite, tests


def expected_case(root, test):
    relative = test.get("expected")
    if not isinstance(relative, str):
        raise TestFailure("test has no expected transcript path")
    path = root / relative
    try:
        case = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TestFailure(f"cannot load expected data {path}: {error}") from error
    if not isinstance(case.get("commands"), list) or not all(
            isinstance(command, str) for command in case["commands"]):
        raise TestFailure(f"invalid commands in {path}")
    if not isinstance(case.get("uart"), str):
        raise TestFailure(f"invalid UART transcript in {path}")
    ranges = case.get("address_ranges", {})
    if not isinstance(ranges, dict):
        raise TestFailure(f"invalid address_ranges in {path}")
    return case


def match_with_ranges(expected, actual, ranges):
    """Compare exactly, except placeholders that accept a range of addresses.

    Each placeholder in `expected` (for example <CODE_ADDRESS>) matches any
    0x-prefixed 8-digit hex address inside [min, max], optionally even only.
    Returns None on success or a reason string.
    """
    pattern = re.escape(expected)
    for name in ranges:
        pattern = pattern.replace(re.escape(name), f"(?P<{re.sub('[^A-Za-z0-9]', '_', name)}>0x[0-9a-f]{{8}})")
    found = re.fullmatch(pattern, actual)
    if not found:
        return "UART mismatch (text outside the accepted address placeholders differs)"
    for name, limits in ranges.items():
        group = re.sub("[^A-Za-z0-9]", "_", name)
        value = int(found.group(group), 16)
        low, high = int(limits["min"], 16), int(limits["max"], 16)
        if not low <= value <= high or (limits.get("even") and value & 1):
            return (f"address {found.group(group)} is not an accepted value "
                    f"(need {limits['min']}..{limits['max']}{', even' if limits.get('even') else ''})")
    return None


def remaining(deadline):
    return max(0.0, deadline - time.monotonic())


def read_until(process, transcript, marker, start, deadline, timeout_reason):
    while transcript.find(marker, start) < 0:
        wait = remaining(deadline)
        if wait == 0:
            raise TestFailure(timeout_reason)
        ready, _, _ = select.select([process.stdout], [], [], wait)
        if not ready:
            raise TestFailure(timeout_reason)
        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            status = process.poll()
            raise TestFailure(f"QEMU exited before the prompt (status {status})")
        transcript.extend(chunk)


def stop_qemu(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=1.0)


def normalize_transport(data):
    # A host serial backend may render the UART's newline as CRLF. No other
    # whitespace, timing-dependent text, addresses, or shell output is changed.
    return data.replace(b"\r\n", b"\n")


def mismatch_reason(expected, actual):
    expected_lines = expected.splitlines(keepends=True)
    actual_lines = actual.splitlines(keepends=True)
    for line_number, (want, got) in enumerate(zip(expected_lines, actual_lines), 1):
        if want != got:
            return f"UART mismatch at line {line_number}: expected {want!r}, got {got!r}"
    if len(expected_lines) != len(actual_lines):
        line_number = min(len(expected_lines), len(actual_lines)) + 1
        want = expected_lines[line_number - 1] if line_number <= len(expected_lines) else "<end>"
        got = actual_lines[line_number - 1] if line_number <= len(actual_lines) else "<end>"
        return f"UART mismatch at line {line_number}: expected {want!r}, got {got!r}"
    return f"UART mismatch: expected {len(expected)} characters, got {len(actual)}"


def run_test(args, root, test, global_deadline):
    case = expected_case(root, test)
    command = [
        args.qemu, "-machine", "mps2-an385", "-monitor", "null", "-nographic",
        "-serial", "stdio", "-kernel", str(args.kernel), "-device",
        f"loader,file={args.image},addr={IMAGE_ADDRESS},force-raw=on",
    ]
    transcript = bytearray()
    with tempfile.TemporaryFile() as stderr:
        try:
            process = subprocess.Popen(
                command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                bufsize=0,
            )
        except OSError as error:
            raise TestFailure(f"cannot start QEMU: {error}") from error
        try:
            startup_deadline = min(global_deadline, time.monotonic() + args.startup_timeout)
            startup_reason = "global timeout during startup" if startup_deadline == global_deadline else "startup timeout waiting for prompt"
            read_until(process, transcript, PROMPT, 0, startup_deadline, startup_reason)
            test_deadline = min(global_deadline, time.monotonic() + args.test_timeout)
            for command_text in case["commands"]:
                try:
                    process.stdin.write(command_text.encode("ascii") + b"\n")
                    process.stdin.flush()
                except (BrokenPipeError, OSError) as error:
                    raise TestFailure(f"cannot send command {command_text!r}: {error}") from error
                before = len(transcript)
                reason = "global timeout" if test_deadline == global_deadline else f"per-test timeout after command {command_text!r}"
                read_until(process, transcript, PROMPT, before, test_deadline, reason)
        except TestFailure as error:
            stop_qemu(process)
            stderr.seek(0)
            detail = stderr.read().decode("utf-8", errors="replace").strip()
            if detail:
                raise TestFailure(f"{error}; QEMU stderr: {detail}") from error
            raise
        finally:
            stop_qemu(process)

    actual_bytes = normalize_transport(bytes(transcript))
    try:
        actual = actual_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise TestFailure(f"UART output is not UTF-8/ASCII at byte {error.start}") from error
    expected = case["uart"]
    ranges = case.get("address_ranges", {})
    if ranges:
        range_error = match_with_ranges(expected, actual, ranges)
        if range_error is None:
            return
        shown = expected
        for name in ranges:
            shown = shown.replace(name, "0x????????")
        # Show a line-level diff with the placeholder left in the expected text.
        diff = "".join(difflib.unified_diff(
            expected.splitlines(keepends=True), actual.splitlines(keepends=True),
            fromfile="expected", tofile="actual", n=1,
        )).rstrip()
        raise TestFailure(f"{range_error}\n{diff}")
    if actual != expected:
        reason = mismatch_reason(expected, actual)
        diff = "".join(difflib.unified_diff(
            expected.splitlines(keepends=True), actual.splitlines(keepends=True),
            fromfile="expected", tofile="actual", n=1,
        )).rstrip()
        raise TestFailure(f"{reason}\n{diff}")


def main():
    args = parse_args()
    root = Path(__file__).resolve().parent
    try:
        suite, tests = load_suite(root, args.suite, args.selected_tests)
        for test in tests:
            expected_case(root, test)
        if args.list:
            print(f"{args.suite}: {suite.get('description', '')}")
            for test in tests:
                print(test["name"])
            return 0
        if args.kernel is None or args.image is None:
            raise TestFailure("--kernel and --image are required unless --list is used")
        for label, value in (("kernel", args.kernel), ("image", args.image)):
            if not value.is_file():
                raise TestFailure(f"{label} is not a file: {value}")
        if min(args.startup_timeout, args.test_timeout, args.global_timeout) <= 0:
            raise TestFailure("timeouts must be greater than zero")
    except TestFailure as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    passed = 0
    failed = 0
    global_deadline = time.monotonic() + args.global_timeout
    for test in tests:
        name = test["name"]
        if remaining(global_deadline) == 0:
            print(f"FAIL {name}: global timeout before test started")
            failed += 1
            continue
        try:
            run_test(args, root, test, global_deadline)
        except TestFailure as error:
            lines = str(error).splitlines()
            print(f"FAIL {name}: {lines[0]}")
            for line in lines[1:]:
                print(f"  {line}")
            failed += 1
        else:
            print(f"PASS {name}")
            passed += 1
    print(f"SUMMARY suite={args.suite} passed={passed} failed={failed} total={len(tests)}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nERROR: interrupted", file=sys.stderr)
        raise SystemExit(130)
