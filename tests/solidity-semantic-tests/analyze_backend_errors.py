#!/usr/bin/env python3
"""Analyze puya backend errors by looking at actual puya stderr output."""
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


def check_one(sol_path: Path) -> tuple[str, str, str]:
    """Check a single test's error output."""
    test = parse_test_file(sol_path)
    if test.skipped:
        return test.name, "SKIP", ""

    out_dir = OUT_DIR / test.category / test.name
    awst_file = out_dir / "awst.json"

    if not awst_file.exists():
        return test.name, "FRONTEND", ""

    arc56_files = list(out_dir.glob("*.arc56.json"))
    if arc56_files:
        return test.name, "OK", ""

    # Re-run puya to capture the error
    options_file = out_dir / "options.json"
    if not options_file.exists():
        return test.name, "FRONTEND", "no options.json"

    try:
        result = subprocess.run(
            [str(PUYA), "--awst", str(awst_file),
             "--options", str(options_file), "--log-level", "info"],
            capture_output=True, text=True, timeout=60,
        )
    except subprocess.TimeoutExpired:
        return test.name, "TIMEOUT", ""

    stderr = result.stderr + result.stdout

    # Extract the key error pattern
    error_lines = []
    for line in stderr.split("\n"):
        if "error:" in line.lower() or "Error" in line:
            error_lines.append(line.strip())

    if error_lines:
        return test.name, "BACKEND", "\n".join(error_lines[:5])

    return test.name, "BACKEND", stderr[-500:] if stderr else "no output"


def categorize_puya_error(error_text: str) -> str:
    """Extract the key error pattern from puya error output."""
    patterns = [
        (r'unknown wtype: (\S+)', 'unknown wtype: {}'),
        (r'cannot resolve .+? type for (.+)', 'type resolution: {}'),
        (r'InternalError: (.+?)(?:\n|$)', 'InternalError: {}'),
        (r'could not determine (\S+ \S+)', 'undetermined: {}'),
        (r'Unexpected (\S+)', 'Unexpected: {}'),
        (r'unsupported (\S+)', 'unsupported: {}'),
        (r'not supported', 'not supported'),
        (r'Invalid (\S+)', 'Invalid: {}'),
    ]

    for pattern, template in patterns:
        m = re.search(pattern, error_text, re.IGNORECASE)
        if m:
            return template.format(m.group(1)[:50]) if '{}' in template else template

    # Find first meaningful error line
    for line in error_text.split("\n"):
        line = line.strip()
        if "error" in line.lower() and len(line) > 10:
            # Remove file paths and line numbers
            cleaned = re.sub(r'/[^\s]+\.py:\d+', '', line)
            cleaned = re.sub(r'File ".*?"', '', cleaned)
            return cleaned[:120]

    return error_text[:100]


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()

    # Find tests with awst.json but no arc56.json (backend failures)
    backend_failures = []
    for cat_dir in sorted(TESTS_DIR.iterdir()):
        if not cat_dir.is_dir():
            continue
        for sol in sorted(cat_dir.glob("*.sol")):
            out_dir = OUT_DIR / cat_dir.name / sol.stem
            awst_file = out_dir / "awst.json"
            arc56_files = list(out_dir.glob("*.arc56.json")) if out_dir.exists() else []
            if awst_file.exists() and not arc56_files:
                backend_failures.append(sol)

    print(f"Found {len(backend_failures)} backend failures to analyze\n")

    error_categories = Counter()

    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(check_one, f): f for f in backend_failures}
        for future in as_completed(futures):
            name, status, detail = future.result()
            if status == "BACKEND" and detail:
                cat = categorize_puya_error(detail)
                error_categories[cat] += 1

    print(f"\nPuya Backend Error Categories ({len(backend_failures)} tests):")
    print("=" * 80)
    for cat, count in error_categories.most_common(40):
        print(f"  {count:>4}x  {cat}")


if __name__ == "__main__":
    main()
