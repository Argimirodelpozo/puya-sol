"""Read a pytest output log and bucket failures by category + root cause.

Run after a `pytest tests/ --tb=no -q > log` to see what's broken and where.

Output: per-category pass/fail counts + a top-N failure-message clustering
so we can spot recurring framework or codegen gaps.

Usage:
    python analyze_baseline.py /tmp/baseline_run.log
"""
from __future__ import annotations

import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


def main():
    if len(sys.argv) < 2:
        print("usage: analyze_baseline.py <pytest_log>")
        sys.exit(1)
    log = Path(sys.argv[1]).read_text()

    # Per-test status lines (verbose mode) — works for -q if pytest converts at the end.
    status_re = re.compile(
        r"^(tests/(\w+)/test_\w+\.py::(\w+))\s+(PASSED|FAILED|ERROR|XFAIL|XPASS|SKIPPED)"
    )
    short_re = re.compile(r"^(?:FAILED|ERROR|XFAIL|XPASS|PASSED)\s+(tests/(\w+)/test_\w+\.py::(\w+))\s*(?:-\s*(.*))?$")

    # Tally results
    per_cat = defaultdict(lambda: Counter())
    fail_reason_per_test = {}

    for line in log.splitlines():
        m = status_re.match(line)
        if m:
            cat = m.group(2)
            outcome = m.group(4)
            per_cat[cat][outcome] += 1
            continue
        m = short_re.match(line)
        if m:
            # Line starts with FAILED/ERROR/etc; classify by the leading word.
            leading = line.split(None, 1)[0]
            test = m.group(1)
            cat = m.group(2)
            reason = (m.group(4) or "").strip()
            fail_reason_per_test[test] = reason
            per_cat[cat][leading] += 1

    # If we got nothing from per-test lines, derive PASS counts from the
    # final summary + per-test FAILED lines (which are emitted by `-q` mode).
    if not any(c.get("PASSED") for c in per_cat.values()):
        m = re.search(r"(\d+) passed", log)
        total_passed = int(m.group(1)) if m else 0
        # Distribute PASS by inverse of FAIL count per category — best-effort.
        # Better: count `.` characters in the progress block, but the order
        # isn't preserved with xdist workers.
        # We'll leave PASSED empty in the per-cat table and just show TOTAL.
        print(f"(Could not derive per-category PASS counts from -q output; "
              f"total passed: {total_passed}, total failed: "
              f"{sum(c.get('FAILED', 0) for c in per_cat.values())})\n")

    if not per_cat:
        # Fallback: try to parse final summary line.
        m = re.search(
            r"(\d+) (?:passed|failed|errored|xfailed)",
            log,
        )
        print("Couldn't parse per-test results from this log.")
        print("Final-line tally summary line found:" if m else "Final summary not found either.")
        if m:
            for hit in re.finditer(r"(\d+) (passed|failed|errored|xfailed|skipped|xpassed)", log):
                print(f"  {hit.group(2)}: {hit.group(1)}")
        sys.exit(0)

    # Per-category table
    print(f"{'Category':<30} {'pass':>6} {'fail':>6} {'err':>6} {'xfail':>6} {'total':>6}")
    print("-" * 64)
    total = Counter()
    for cat in sorted(per_cat):
        c = per_cat[cat]
        cat_total = sum(c.values())
        total += c
        print(
            f"{cat:<30} {c['PASSED']:>6} {c['FAILED']:>6} {c['ERROR']:>6} {c['XFAIL']:>6} {cat_total:>6}"
        )
    print("-" * 64)
    print(
        f"{'TOTAL':<30} {total['PASSED']:>6} {total['FAILED']:>6} "
        f"{total['ERROR']:>6} {total['XFAIL']:>6} {sum(total.values()):>6}"
    )

    # Top reasons — categorize by exception class + first phrase
    print("\nTop failure root-cause clusters:")
    reason_counts = Counter()
    for r in fail_reason_per_test.values():
        if not r:
            continue
        # Pytest -q truncates to 'FAILED test - ExceptionClass: ...'
        # We extract the exception class + first 40 chars of message.
        m = re.match(r"^(\w+(?:\.\w+)*Error|\w+Error)[\s:]*(.*)", r)
        if m:
            klass = m.group(1)
            msg = m.group(2)
            # Strip dynamic values that hurt clustering
            msg = re.sub(r"\b0x[0-9a-fA-F]+\b", "0xN", msg)
            msg = re.sub(r"\b\d{4,}\b", "N", msg)
            key = f"{klass}: {msg[:60]}"
        else:
            key = r[:80]
        reason_counts[key] += 1
    for reason, n in reason_counts.most_common(25):
        print(f"  {n:>4}  {reason}")


if __name__ == "__main__":
    main()
