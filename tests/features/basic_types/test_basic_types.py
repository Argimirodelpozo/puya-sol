"""Regression tests for basic Solidity types: uint256, bool, address, bytes32, int256."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, NO_POPULATE

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "BasicTypes.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["BasicTypes"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestCompilation:
    def test_compiles(self, compiled):
        assert "BasicTypes" in compiled
        assert compiled["BasicTypes"]["approval_bin"].exists()

    def test_within_size_limit(self, compiled):
        size = compiled["BasicTypes"]["approval_bin"].stat().st_size
        assert size <= 8192


class TestUint256:
    def test_default_zero(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="getUint"))
        assert r.abi_return == 0

    def test_set_and_get(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setUint", args=[42]))
        r = app.send.call(au.AppClientMethodCallParams(method="getUint"))
        assert r.abi_return == 42

    def test_large_value(self, app):
        big = 2**200
        app.send.call(au.AppClientMethodCallParams(method="setUint", args=[big]))
        r = app.send.call(au.AppClientMethodCallParams(method="getUint"))
        assert r.abi_return == big

    def test_add_pure(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="addUints", args=[100, 200]))
        assert r.abi_return == 300


class TestBool:
    def test_default_false(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="getBool"))
        assert r.abi_return is False

    def test_set_true(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setBool", args=[True]))
        r = app.send.call(au.AppClientMethodCallParams(method="getBool"))
        assert r.abi_return is True

    def test_and(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="boolAnd", args=[True, True])).abi_return is True
        assert app.send.call(au.AppClientMethodCallParams(method="boolAnd", args=[True, False])).abi_return is False

    def test_or(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="boolOr", args=[False, True])).abi_return is True
        assert app.send.call(au.AppClientMethodCallParams(method="boolOr", args=[False, False])).abi_return is False

    def test_not(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="boolNot", args=[True])).abi_return is False
        assert app.send.call(au.AppClientMethodCallParams(method="boolNot", args=[False])).abi_return is True


class TestAddress:
    def test_identity(self, app, account):
        r = app.send.call(au.AppClientMethodCallParams(method="identityAddress", args=[account.address]))
        assert r.abi_return == account.address

    def test_store_and_read(self, app, account):
        app.send.call(au.AppClientMethodCallParams(method="setAddr", args=[account.address]))
        r = app.send.call(au.AppClientMethodCallParams(method="getAddr"))
        assert r.abi_return == account.address


class TestBytes32:
    def test_identity(self, app):
        val = list(range(32))  # [0, 1, 2, ..., 31]
        r = app.send.call(au.AppClientMethodCallParams(method="identityBytes32", args=[val]))
        # Return might be bytes or list[int]
        if isinstance(r.abi_return, bytes):
            assert r.abi_return == bytes(val)
        else:
            assert list(r.abi_return) == val
