#!/usr/bin/env python3
"""Quick compile-only baseline for all semantic tests.

Classifies each test as: SKIP | COMPILE_OK | COMPILE_ERROR
Does NOT deploy or run — just checks if puya-sol can compile each .sol file.

Usage: python compile_baseline.py [--workers 8]
"""
import subprocess
import sys
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed

from parser import parse_test_file

ROOT = Path(__file__).parent.parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT / "puya" / ".venv" / "bin" / "puya"
TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"


def compile_one(sol_path: Path) -> tuple[str, str, str, str]:
    """Try to compile a single .sol file. Returns (category, name, status, detail)."""
    test = parse_test_file(sol_path)
    category = test.category
    name = test.name

    if test.skipped:
        return category, name, "SKIP", test.skip_reason

    out_dir = OUT_DIR / category / name
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [str(COMPILER), "--source", str(sol_path),
           "--output-dir", str(out_dir),
           "--puya-path", str(PUYA)]
    if test.compile_via_yul:
        cmd += ["--via-yul-behavior"]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return category, name, "COMPILE_ERROR", "timeout"

    if result.returncode != 0:
        # Extract first error line
        stderr = result.stderr.strip()
        first_line = stderr.split("\n")[0][:120] if stderr else "unknown error"
        return category, name, "COMPILE_ERROR", first_line

    # Check we got at least one deployable contract
    arc56_files = list(out_dir.glob("*.arc56.json"))
    if not arc56_files:
        return category, name, "COMPILE_ERROR", "no arc56 output"

    return category, name, "COMPILE_OK", f"{len(arc56_files)} contract(s)"


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()

    # Gather all .sol files
    sol_files = []
    for cat_dir in sorted(TESTS_DIR.iterdir()):
        if not cat_dir.is_dir():
            continue
        for sol in sorted(cat_dir.glob("*.sol")):
            sol_files.append(sol)

    print(f"Found {len(sol_files)} test files across {len(set(f.parent.name for f in sol_files))} categories\n")

    results = {"SKIP": 0, "COMPILE_OK": 0, "COMPILE_ERROR": 0}
    category_results = {}
    error_reasons = {}

    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(compile_one, f): f for f in sol_files}
        done = 0
        for future in as_completed(futures):
            done += 1
            cat, name, status, detail = future.result()
            results[status] += 1

            if cat not in category_results:
                category_results[cat] = {"SKIP": 0, "COMPILE_OK": 0, "COMPILE_ERROR": 0}
            category_results[cat][status] += 1

            if status == "COMPILE_ERROR":
                # Bucket error reasons
                bucket = detail.split(":")[0] if ":" in detail else detail[:60]
                error_reasons[bucket] = error_reasons.get(bucket, 0) + 1

            if done % 50 == 0:
                print(f"  Progress: {done}/{len(sol_files)} "
                      f"(OK={results['COMPILE_OK']} ERR={results['COMPILE_ERROR']} SKIP={results['SKIP']})")

    # Print per-category summary
    print(f"\n{'Category':<30} {'OK':>5} {'ERR':>5} {'SKIP':>5} {'Total':>5}")
    print("-" * 80)
    for cat in sorted(category_results):
        r = category_results[cat]
        total = sum(r.values())
        print(f"{cat:<30} {r['COMPILE_OK']:>5} {r['COMPILE_ERROR']:>5} {r['SKIP']:>5} {total:>5}")

    # Print overall summary
    total = sum(results.values())
    print(f"\n{'='*50}")
    print(f"Total: {total} tests")
    for status in ["COMPILE_OK", "COMPILE_ERROR", "SKIP"]:
        count = results[status]
        pct = count / total * 100 if total else 0
        print(f"  {status}: {count} ({pct:.1f}%)")

    # Print top error buckets
    if error_reasons:
        print(f"\nTop compile error patterns:")
        for reason, count in sorted(error_reasons.items(), key=lambda x: -x[1])[:20]:
            print(f"  {count:>4}x  {reason}")


if __name__ == "__main__":
    main()
