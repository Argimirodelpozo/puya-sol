"""Filesystem paths used across the framework."""
import os
from pathlib import Path

# .../puya-sol/tests/solidity-semantic-tests/framework/paths.py
# parents[0]=framework, [1]=solidity-semantic-tests, [2]=tests, [3]=puya-sol (repo)
FRAMEWORK_DIR = Path(__file__).resolve().parent
SEMANTIC_TESTS_DIR = FRAMEWORK_DIR.parent
PUYA_SOL_ROOT = SEMANTIC_TESTS_DIR.parent.parent

COMPILER = PUYA_SOL_ROOT / "build" / "puya-sol"
# PUYA_SOL_PUYA points the whole suite at an alternate puya backend (e.g. a
# vanilla-upstream worktree to measure a version bump before syncing the
# fork). The backend cache signature hashes PUYA_SOL_PUYA_SRC, so alternate
# backends never share cached artifacts with the fork's.
PUYA = Path(os.environ.get("PUYA_SOL_PUYA")
            or PUYA_SOL_ROOT / "puya" / ".venv" / "bin" / "puya")

TESTS_DIR = SEMANTIC_TESTS_DIR / "tests"
OUT_DIR = SEMANTIC_TESTS_DIR / "out"
CACHE_DIR = SEMANTIC_TESTS_DIR / ".compile_cache"
PUYA_BACKEND_SRC = Path(os.environ.get("PUYA_SOL_PUYA_SRC")
                        or PUYA_SOL_ROOT / "puya" / "src")
