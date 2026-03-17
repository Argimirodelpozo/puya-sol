"""Regression tests for Solidity enums."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Enums.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Enums"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestEnums:
    def test_default_pending(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="isPending")).abi_return is True

    def test_set_active(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setStatus", args=[1]))
        assert app.send.call(au.AppClientMethodCallParams(method="isActive")).abi_return is True

    def test_to_uint(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setStatus", args=[2]))
        assert app.send.call(au.AppClientMethodCallParams(method="statusToUint")).abi_return == 2
