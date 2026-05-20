"""Filesystem paths used across the framework."""
from pathlib import Path

# .../puya-sol/tests/solidity-semantic-tests/framework/paths.py
# parents[0]=framework, [1]=solidity-semantic-tests, [2]=tests, [3]=puya-sol (repo)
FRAMEWORK_DIR = Path(__file__).resolve().parent
SEMANTIC_TESTS_DIR = FRAMEWORK_DIR.parent
PUYA_SOL_ROOT = SEMANTIC_TESTS_DIR.parent.parent

COMPILER = PUYA_SOL_ROOT / "build" / "puya-sol"
PUYA = PUYA_SOL_ROOT / "puya" / ".venv" / "bin" / "puya"

TESTS_DIR = SEMANTIC_TESTS_DIR / "tests"
OUT_DIR = SEMANTIC_TESTS_DIR / "out"
CACHE_DIR = SEMANTIC_TESTS_DIR / ".compile_cache"
PUYA_BACKEND_SRC = PUYA_SOL_ROOT / "puya" / "src"
