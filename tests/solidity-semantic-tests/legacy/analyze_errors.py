#!/usr/bin/env python3
"""Analyze compile errors in more detail — look at stderr from failed compilations."""
import subprocess
import sys
import re
from pathlib import Path
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, as_completed

from parser import parse_test_file

ROOT = Path(__file__).parent.parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT / "puya" / ".venv" / "bin" / "puya"
TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"


def analyze_one(sol_path: Path) -> tuple[str, str, str, str]:
    """Compile and capture detailed error info."""
    test = parse_test_file(sol_path)
    if test.skipped:
        return test.category, test.name, "SKIP", test.skip_reason

    out_dir = OUT_DIR / test.category / test.name
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [str(COMPILER), "--source", str(sol_path),
           "--output-dir", str(out_dir),
           "--puya-path", str(PUYA)]
    if test.compile_via_yul:
        cmd += ["--via-yul-behavior"]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return test.category, test.name, "TIMEOUT", ""

    if result.returncode != 0:
        stderr = result.stderr + result.stdout
        return test.category, test.name, "ERROR", stderr[:2000]

    arc56_files = list(out_dir.glob("*.arc56.json"))
    if not arc56_files:
        # Check if puya-sol succeeded but puya backend failed
        # Look for awst.json to distinguish frontend vs backend errors
        awst_file = out_dir / "awst.json"
        if awst_file.exists():
            return test.category, test.name, "PUYA_BACKEND_ERROR", result.stderr[:2000]
        return test.category, test.name, "FRONTEND_ERROR", result.stderr[:2000]

    return test.category, test.name, "OK", ""


def categorize_error(stderr: str) -> str:
    """Extract the key error pattern from stderr."""
    # Check for puya-sol (frontend) errors
    if "Unsupported" in stderr:
        m = re.search(r'Unsupported[^:]*:\s*(.+?)(?:\n|$)', stderr)
        return f"Unsupported: {m.group(1)[:60]}" if m else "Unsupported (generic)"

    if "not yet supported" in stderr.lower():
        m = re.search(r'(?:not yet supported|Not yet supported)[^:]*:\s*(.+?)(?:\n|$)', stderr)
        return f"Not yet supported: {m.group(1)[:60]}" if m else "Not yet supported (generic)"

    if "not supported" in stderr.lower():
        m = re.search(r'(?:not supported|Not supported)[^:]*:\s*(.+?)(?:\n|$)', stderr)
        return f"Not supported: {m.group(1)[:60]}" if m else "Not supported (generic)"

    if "Error" in stderr:
        # Find first error line
        for line in stderr.split("\n"):
            if "Error" in line and len(line) > 5:
                return line.strip()[:100]

    if "error" in stderr.lower():
        for line in stderr.split("\n"):
            if "error" in line.lower() and len(line) > 5:
                return line.strip()[:100]

    if "assert" in stderr.lower():
        for line in stderr.split("\n"):
            if "assert" in line.lower():
                return line.strip()[:100]

    return stderr.split("\n")[0][:100] if stderr else "unknown"


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()

    sol_files = []
    for cat_dir in sorted(TESTS_DIR.iterdir()):
        if not cat_dir.is_dir():
            continue
        for sol in sorted(cat_dir.glob("*.sol")):
            sol_files.append(sol)

    print(f"Analyzing {len(sol_files)} test files...\n")

    results = Counter()
    error_patterns = Counter()
    frontend_errors = Counter()
    backend_errors = Counter()

    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(analyze_one, f): f for f in sol_files}
        done = 0
        for future in as_completed(futures):
            done += 1
            cat, name, status, detail = future.result()
            results[status] += 1

            if status in ("ERROR", "FRONTEND_ERROR", "PUYA_BACKEND_ERROR"):
                pattern = categorize_error(detail)
                error_patterns[pattern] += 1
                if status == "FRONTEND_ERROR":
                    frontend_errors[pattern] += 1
                elif status == "PUYA_BACKEND_ERROR":
                    backend_errors[pattern] += 1

            if done % 100 == 0:
                print(f"  {done}/{len(sol_files)}")

    print(f"\n{'='*60}")
    print(f"Total: {sum(results.values())} tests")
    for status, count in results.most_common():
        pct = count / sum(results.values()) * 100
        print(f"  {status}: {count} ({pct:.1f}%)")

    if frontend_errors:
        print(f"\n--- Frontend Errors (puya-sol) ---")
        for pattern, count in frontend_errors.most_common(20):
            print(f"  {count:>4}x  {pattern}")

    if backend_errors:
        print(f"\n--- Backend Errors (puya) ---")
        for pattern, count in backend_errors.most_common(20):
            print(f"  {count:>4}x  {pattern}")

    if error_patterns:
        print(f"\n--- All Error Patterns ---")
        for pattern, count in error_patterns.most_common(30):
            print(f"  {count:>4}x  {pattern}")


if __name__ == "__main__":
    main()
