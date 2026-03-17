"""Regression tests for type conversions and casts."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "TypeConversions.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["TypeConversions"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestWidening:
    def test_uint64_to_uint256(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="uint64ToUint256", args=[42])).abi_return == 42


class TestNarrowing:
    def test_uint256_to_uint64(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="uint256ToUint64", args=[100])).abi_return == 100

    def test_uint256_to_uint64_truncation(self, app):
        # 2^64 + 1 should truncate to 1
        val = 2**64 + 1
        assert app.send.call(au.AppClientMethodCallParams(method="uint256ToUint64", args=[val])).abi_return == 1

    def test_narrow_uint160(self, app):
        big = 2**200
        masked = big & (2**160 - 1)
        assert app.send.call(au.AppClientMethodCallParams(method="narrowUint256ToUint160", args=[big])).abi_return == masked


class TestBytesConversion:
    def test_uint256_to_bytes32(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="uint256ToBytes32", args=[1]))
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert result == b"\x00" * 31 + b"\x01"

    def test_bytes32_to_uint256(self, app):
        val = [0] * 31 + [42]
        assert app.send.call(au.AppClientMethodCallParams(method="bytes32ToUint256", args=[val])).abi_return == 42


class TestTypeMax:
    def test_max_uint256(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="maxUint256")).abi_return == 2**256 - 1

    def test_max_uint64(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="maxUint64")).abi_return == 2**64 - 1
