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
    rep_call = getattr(request.node, "rep_call", None)
    if rep_call is not None and rep_call.passed:
        h.cleanup()


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


# Auto-reset thresholds (see pytest_collection_finish): a collection at least
# this large is a "full-suite-ish" run…
_LOCALNET_RESET_AUTO_THRESHOLD = 300
# …but the reset itself costs ~100 s (docker restart + cold algod), while the
# ledger only grows ~25 MB / ~7.5k rounds per full run — so only reset once the
# ledger has actually aged. ~50k rounds ≈ 6-7 full runs of accumulation; the
# measured death spiral (2.1 GB ledger, +600 s/run) took dozens of runs.
_LOCALNET_RESET_ROUND_THRESHOLD = 50_000


def _localnet_last_round() -> int | None:
    """Current round of the local algod via REST (algokit default port/token);
    None if unreachable."""
    import json, urllib.request
    try:
        req = urllib.request.Request(
            "http://localhost:4001/v2/status",
            headers={"X-Algo-API-Token": "a" * 64},
        )
        with urllib.request.urlopen(req, timeout=5) as resp:
            return int(json.load(resp).get("last-round", 0))
    except Exception:
        return None


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
              (>= _LOCALNET_RESET_AUTO_THRESHOLD tests) AND the ledger has
              aged past _LOCALNET_RESET_ROUND_THRESHOLD rounds. A young
              localnet skips the ~100 s reset entirely; an aged one pays it
              once and recovers the fast floor. Small selections never reset.

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
        return
    if flag is None and n >= _LOCALNET_RESET_AUTO_THRESHOLD:
        rounds = _localnet_last_round()
        if rounds is None or rounds >= _LOCALNET_RESET_ROUND_THRESHOLD:
            _reset_localnet(
                f"auto: {n} tests, ledger at round {rounds} >= {_LOCALNET_RESET_ROUND_THRESHOLD}")
        else:
            print(f"[localnet-reset] skipped: ledger young (round {rounds} < "
                  f"{_LOCALNET_RESET_ROUND_THRESHOLD})", flush=True)


def pytest_collection_modifyitems(config, items):
    """Tag tests by category for filtering with -m cat_<name>."""
    for item in items:
        parts = item.nodeid.split("/")
        if "tests" in parts:
            idx = parts.index("tests")
            if idx + 1 < len(parts):
                category = parts[idx + 1]
                item.add_marker(getattr(pytest.mark, f"cat_{category}"))
