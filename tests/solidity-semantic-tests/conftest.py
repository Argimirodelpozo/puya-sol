"""Pytest fixtures for the puya-sol semantic test suite.

Architecture:
  framework/      — shared library (compile, deploy, call, harness).
  tests/<cat>/    — Solidity test fixtures + a single test_<cat>.py per category.

This conftest exposes a `harness` fixture: each test gets a fresh output
directory, a per-test Harness, and shares one session-scoped LocalNet
client + funded dispenser account.

To run:
    pytest tests/                       # everything
    pytest tests/smoke                  # one category
    pytest tests/smoke -k alignment     # one test
    pytest -n 8 tests/                  # parallel via pytest-xdist

Old infrastructure (run_tests.py, the old conftest, etc.) lives under
`legacy/` and is no longer pytest-discovered.
"""
from __future__ import annotations

import os
import shutil
from pathlib import Path

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

    The output dir is named after the nodeid so collisions don't happen
    even with parallel xdist workers (`-n auto`).
    """
    nodeid = request.node.nodeid.replace("/", "_").replace("::", "_").replace("[", "_").replace("]", "_")
    test_out = OUT_DIR / nodeid
    if test_out.exists():
        shutil.rmtree(test_out, ignore_errors=True)
    h = Harness(localnet, test_out)
    yield h
    # Keep out dir on failure for post-mortem; clean on pass.
    if not request.node.rep_call.failed if hasattr(request.node, "rep_call") else True:
        # No-op if rep_call isn't set (skip / error during collection).
        pass


# Capture per-test outcome so the harness fixture can decide whether to
# preserve the output dir for debugging.
@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)


def pytest_configure(config):
    """Pre-register per-category markers so filtering with `-m cat_smoke` works
    without the PytestUnknownMarkWarning noise."""
    from framework.paths import TESTS_DIR
    for d in TESTS_DIR.iterdir():
        if d.is_dir():
            config.addinivalue_line("markers", f"cat_{d.name}: tests under tests/{d.name}/")


# Auto-reset threshold: a collection at least this large is a "full-suite-ish"
# run and gets a fresh localnet by default (see pytest_collection_finish).
_LOCALNET_RESET_AUTO_THRESHOLD = 300


def _reset_localnet(reason: str) -> None:
    import subprocess
    try:
        r = subprocess.run(
            ["algokit", "localnet", "reset"],
            capture_output=True, text=True, timeout=180,
        )
        if r.returncode != 0:
            print(f"[localnet-reset] failed (rc={r.returncode}): {r.stderr[:200]}",
                  flush=True)
        else:
            print(f"[localnet-reset] OK ({reason})", flush=True)
    except FileNotFoundError:
        print("[localnet-reset] algokit not on PATH; skipped", flush=True)
    except subprocess.TimeoutExpired:
        print("[localnet-reset] timed out; continuing on the old ledger", flush=True)


def pytest_collection_finish(session):
    """Reset the algokit localnet before large runs (default-on).

    A long-lived localnet accumulates rounds/apps/boxes — a multi-day ledger
    measured 2.1 GB and made identical full-suite runs drift from ~7 min to
    ~17 min (every simulate/confirm scans the growing state). Resetting keeps
    the app-id space small and run timings comparable.

    Policy (PUYASOL_LOCALNET_RESET):
      "1"   → always reset, any run size.
      "0"   → never reset.
      unset → reset automatically when the collected run is large
              (>= _LOCALNET_RESET_AUTO_THRESHOLD tests); single tests and
              small selections never pay the ~20 s reset.

    No-op under pytest-xdist worker processes (only the master resets;
    workers see the already-reset state).
    """
    import os
    if hasattr(session.config, "workerinput"):
        return
    flag = os.environ.get("PUYASOL_LOCALNET_RESET")
    if flag == "0":
        return
    n = len(getattr(session, "items", []) or [])
    if flag == "1":
        _reset_localnet(f"forced by env; {n} tests collected")
    elif flag is None and n >= _LOCALNET_RESET_AUTO_THRESHOLD:
        _reset_localnet(f"auto: {n} tests collected >= {_LOCALNET_RESET_AUTO_THRESHOLD}")


def pytest_collection_modifyitems(config, items):
    """Tag tests by category for filtering with -m cat_<name>."""
    for item in items:
        parts = item.nodeid.split("/")
        if "tests" in parts:
            idx = parts.index("tests")
            if idx + 1 < len(parts):
                category = parts[idx + 1]
                item.add_marker(getattr(pytest.mark, f"cat_{category}"))
