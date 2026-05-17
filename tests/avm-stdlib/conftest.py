"""Pytest config for tests/avm-stdlib/.

Reuses the semantic-test harness: same Harness fixture, same compile +
deploy + call machinery, same multi-source splitter. The framework module
lives under tests/solidity-semantic-tests/framework/; we add that to
sys.path here and adopt its conftest.py fixtures by import.
"""
from __future__ import annotations

import sys
from pathlib import Path

SEMANTIC_TESTS_DIR = Path(__file__).resolve().parent.parent / "solidity-semantic-tests"
sys.path.insert(0, str(SEMANTIC_TESTS_DIR))

import shutil
import pytest

from framework import Harness
from framework.localnet import LocalNet
from framework.paths import OUT_DIR


@pytest.fixture(scope="session")
def localnet() -> LocalNet:
    return LocalNet()


@pytest.fixture
def harness(localnet: LocalNet, request: pytest.FixtureRequest):
    """Per-test Harness with a fresh output directory.

    Mirrors tests/solidity-semantic-tests/conftest.py — the avm-stdlib
    fixtures live one directory up but use the same isolated-output-dir
    pattern so parallel xdist workers don't collide.
    """
    nodeid = (
        request.node.nodeid.replace("/", "_").replace("::", "_").replace("[", "_").replace("]", "_")
    )
    test_out = OUT_DIR / nodeid
    if test_out.exists():
        shutil.rmtree(test_out, ignore_errors=True)
    h = Harness(localnet, test_out)
    yield h


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)
