"""Regression tests for bitwise operations on uint256 and bytes32."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Bitwise.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Bitwise"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestUint256Bitwise:
    def test_and(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="bitwiseAnd", args=[0xFF, 0x0F])).abi_return == 0x0F

    def test_or(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="bitwiseOr", args=[0xF0, 0x0F])).abi_return == 0xFF

    def test_xor(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="bitwiseXor", args=[0xFF, 0x0F])).abi_return == 0xF0

    def test_not(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="bitwiseNot", args=[0]))
        assert r.abi_return == 2**256 - 1

    def test_shift_left(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="shiftLeft", args=[1, 10])).abi_return == 1024

    def test_shift_right(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="shiftRight", args=[1024, 5])).abi_return == 32


class TestBytes32Bitwise:
    def test_and(self, app):
        a = [0xFF] * 16 + [0x00] * 16
        b = [0x00] * 16 + [0xFF] * 16
        r = app.send.call(au.AppClientMethodCallParams(method="bytes32And", args=[a, b]))
        expected = bytes([0x00] * 32)
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert result == expected

    def test_or(self, app):
        a = [0xFF] * 16 + [0x00] * 16
        b = [0x00] * 16 + [0xFF] * 16
        r = app.send.call(au.AppClientMethodCallParams(method="bytes32Or", args=[a, b]))
        expected = bytes([0xFF] * 32)
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert result == expected
