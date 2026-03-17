"""Regression tests for Solidity events (emit, indexed params)."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Events.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Events"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestEvents:
    def test_emit_transfer_no_revert(self, app, account):
        """emitTransfer should execute without error (event logged)."""
        app.send.call(au.AppClientMethodCallParams(
            method="emitTransfer", args=[account.address, account.address, 100],
        ))

    def test_emit_simple_no_revert(self, app):
        app.send.call(au.AppClientMethodCallParams(method="emitSimple", args=[42]))
