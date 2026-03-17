#!/usr/bin/env python3
"""Fetch Solidity semantic tests from the ethereum/solidity GitHub repo.

Downloads .sol test files from test/libsolidity/semanticTests/ and saves
them in a mirrored folder structure under tests/solidity-semantic-tests/tests/.

Usage:
    python fetch_tests.py [--categories cat1,cat2,...] [--limit N]

Categories to focus on (most relevant for puya-sol):
    smoke, constants, arithmetics, expressions, exponentiation, operators,
    enums, structs, getters, immutable, inheritance, modifiers, variables,
    constructor, freeFunctions, functionCall, virtualFunctions, using,
    userDefinedValueType, various, array, scoping, statements,
    conversions, integer, types, literals, strings
"""
import argparse
import base64
import json
import subprocess
import sys
from pathlib import Path

TESTS_DIR = Path(__file__).parent / "tests"

# Categories most likely to work with puya-sol (no EVM-specific features)
DEFAULT_CATEGORIES = [
    "smoke",
    "constants",
    "arithmetics",
    "expressions",
    "exponentiation",
    "operators",
    "enums",
    "getters",
    "immutable",
    "inheritance",
    "modifiers",
    "variables",
    "constructor",
    "freeFunctions",
    "functionCall",
    "virtualFunctions",
    "using",
    "userDefinedValueType",
    "various",
    "scoping",
    "statements",
    "conversions",
    "integer",
    "types",
    "literals",
    "strings",
    "structs",
    "array",
]


def gh_api(path):
    """Call GitHub API and return parsed JSON."""
    result = subprocess.run(
        ["gh", "api", f"repos/ethereum/solidity/contents/{path}"],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0:
        return None
    return json.loads(result.stdout)


def fetch_category(category, limit=None):
    """Fetch all .sol files in a category."""
    base_path = f"test/libsolidity/semanticTests/{category}"
    items = gh_api(base_path)
    if not items:
        print(f"  [SKIP] Could not list {category}")
        return 0

    out_dir = TESTS_DIR / category
    out_dir.mkdir(parents=True, exist_ok=True)

    count = 0
    for item in items:
        if not item["name"].endswith(".sol"):
            continue
        if limit and count >= limit:
            break

        # Fetch file content
        file_data = gh_api(item["path"])
        if not file_data or "content" not in file_data:
            continue

        content = base64.b64decode(file_data["content"]).decode("utf-8", errors="replace")
        out_path = out_dir / item["name"]
        out_path.write_text(content)
        count += 1

    return count


def main():
    parser = argparse.ArgumentParser(description="Fetch Solidity semantic tests")
    parser.add_argument("--categories", default=",".join(DEFAULT_CATEGORIES),
                        help="Comma-separated list of categories")
    parser.add_argument("--limit", type=int, default=None,
                        help="Max tests per category")
    args = parser.parse_args()

    categories = args.categories.split(",")
    total = 0

    for cat in categories:
        cat = cat.strip()
        print(f"Fetching {cat}...", end=" ", flush=True)
        n = fetch_category(cat, args.limit)
        print(f"{n} tests")
        total += n

    print(f"\nTotal: {total} tests fetched to {TESTS_DIR}")


if __name__ == "__main__":
    main()
