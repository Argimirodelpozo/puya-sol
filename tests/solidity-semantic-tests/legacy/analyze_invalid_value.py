#!/usr/bin/env python3
"""Dig deeper into the 'Invalid value' errors from puya backend."""
import subprocess
import re
from pathlib import Path
from collections import Counter

TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"
PUYA = Path(__file__).parent.parent.parent / "puya" / ".venv" / "bin" / "puya"


def main():
    error_details = Counter()
    total = 0

    for cat_dir in sorted(OUT_DIR.iterdir()):
        if not cat_dir.is_dir():
            continue
        for test_dir in sorted(cat_dir.iterdir()):
            if not test_dir.is_dir():
                continue
            awst = test_dir / "awst.json"
            options = test_dir / "options.json"
            arc56_files = list(test_dir.glob("*.arc56.json"))

            if not awst.exists() or arc56_files:
                continue

            # Re-run puya and capture error
            try:
                result = subprocess.run(
                    [str(PUYA), "--awst", str(awst), "--options", str(options),
                     "--log-level", "info"],
                    capture_output=True, text=True, timeout=30,
                )
            except subprocess.TimeoutExpired:
                continue

            output = result.stderr + result.stdout

            # Find all error lines
            for line in output.split("\n"):
                if "Invalid value" in line or "invalid value" in line.lower():
                    # Extract the error detail
                    # Strip ANSI codes
                    clean = re.sub(r'\x1b\[[0-9;]*m', '', line).strip()
                    # Extract after "error:"
                    m = re.search(r'error:\s*(.*)', clean)
                    if m:
                        detail = m.group(1)[:120]
                        error_details[detail] += 1
                        total += 1
                        break
                elif "error:" in line.lower():
                    clean = re.sub(r'\x1b\[[0-9;]*m', '', line).strip()
                    m = re.search(r'error:\s*(.*)', clean)
                    if m:
                        detail = m.group(1)[:120]
                        if total < 600:  # Capture non-invalid-value errors too
                            error_details[detail] += 1
                            total += 1
                            break

            if total >= 700:
                break

    print(f"Analyzed {total} backend errors:\n")
    for detail, count in error_details.most_common(40):
        print(f"  {count:>4}x  {detail}")


if __name__ == "__main__":
    main()
