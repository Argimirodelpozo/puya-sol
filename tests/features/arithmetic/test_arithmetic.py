"""Regression tests for arithmetic operations."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, NO_POPULATE

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Arithmetic.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Arithmetic"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestBasicArithmetic:
    def test_add(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="add", args=[3, 7])).abi_return == 10

    def test_sub(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="sub", args=[10, 4])).abi_return == 6

    def test_mul(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="mul", args=[6, 7])).abi_return == 42

    def test_div(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="div", args=[100, 3])).abi_return == 33

    def test_mod(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="mod", args=[10, 3])).abi_return == 1

    def test_add_large(self, app):
        a = 10**50
        b = 10**50
        assert app.send.call(au.AppClientMethodCallParams(method="add", args=[a, b])).abi_return == 2 * 10**50


class TestUnchecked:
    def test_add_unchecked(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="addUnchecked", args=[5, 10])).abi_return == 15

    def test_sub_unchecked_wrapping(self, app):
        # On AVM biguint, this doesn't actually wrap — biguint is arbitrary precision
        # But Solidity unchecked wraps mod 2^256
        r = app.send.call(au.AppClientMethodCallParams(method="subUnchecked", args=[0, 1]))
        # Should wrap to 2^256 - 1
        assert r.abi_return == 2**256 - 1


class TestSigned:
    """Signed arithmetic uses two's complement on uint256.
    ARC4 ABI uses uint256 encoding — negative values encoded as 2^256 + val."""

    def _to_twos(self, val):
        """Convert signed int to two's complement uint256."""
        return val if val >= 0 else (2**256 + val)

    def _from_twos(self, val):
        """Convert two's complement uint256 back to signed int."""
        if val >= 2**255:
            return val - 2**256
        return val

    def test_add_signed_positive(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="addSigned", args=[5, 3]))
        assert self._from_twos(r.abi_return) == 8

    def test_add_signed_negative(self, app):
        # 5 + (-3) = 2, sent as 5 + (2^256-3)
        r = app.send.call(au.AppClientMethodCallParams(
            method="addSigned", args=[5, self._to_twos(-3)],
        ))
        assert self._from_twos(r.abi_return) == 2

    def test_negate(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="negateSigned", args=[42]))
        assert self._from_twos(r.abi_return) == -42

    def test_sub_signed(self, app):
        # 3 - 7 = -4, sent as sub(3, 7)
        r = app.send.call(au.AppClientMethodCallParams(method="subSigned", args=[3, 7]))
        assert self._from_twos(r.abi_return) == -4


class TestIncrementDecrement:
    def test_increment(self, app):
        app.send.call(au.AppClientMethodCallParams(method="resetCounter"))
        r = app.send.call(au.AppClientMethodCallParams(method="increment"))
        assert r.abi_return == 1

    def test_pre_increment(self, app):
        app.send.call(au.AppClientMethodCallParams(method="resetCounter"))
        r = app.send.call(au.AppClientMethodCallParams(method="preIncrement"))
        assert r.abi_return == 1
