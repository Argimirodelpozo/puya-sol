"""Regression tests for comparison operators."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Comparison.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Comparison"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestUintComparison:
    def test_eq_true(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="eq", args=[5, 5])).abi_return is True

    def test_eq_false(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="eq", args=[5, 6])).abi_return is False

    def test_ne(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="ne", args=[5, 6])).abi_return is True

    def test_lt(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="lt", args=[3, 5])).abi_return is True
        assert app.send.call(au.AppClientMethodCallParams(method="lt", args=[5, 3])).abi_return is False

    def test_lte(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="lte", args=[5, 5])).abi_return is True
        assert app.send.call(au.AppClientMethodCallParams(method="lte", args=[6, 5])).abi_return is False

    def test_gt(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="gt", args=[10, 5])).abi_return is True

    def test_gte(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="gte", args=[5, 5])).abi_return is True

    def test_large_values(self, app):
        big = 2**200
        assert app.send.call(au.AppClientMethodCallParams(method="lt", args=[big, big + 1])).abi_return is True


class TestOtherTypeComparison:
    def test_address_eq(self, app, account):
        assert app.send.call(au.AppClientMethodCallParams(
            method="eqAddr", args=[account.address, account.address]
        )).abi_return is True

    def test_bool_eq(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="eqBool", args=[True, True])).abi_return is True
        assert app.send.call(au.AppClientMethodCallParams(method="eqBool", args=[True, False])).abi_return is False

    def test_bytes32_eq(self, app):
        val = [0xAB] * 32
        assert app.send.call(au.AppClientMethodCallParams(method="eqBytes32", args=[val, val])).abi_return is True
