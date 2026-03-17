"""Regression tests for ABI encoding, multiple returns, and hashing."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "AbiEncoding.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["AbiEncoding"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestAbiEncode:
    def test_encode_uint(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="encodeUint", args=[42]))
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        # ABI-encoded uint256(42) = 32 bytes, big-endian
        assert len(result) == 32
        assert int.from_bytes(result, "big") == 42

    def test_encode_two_uints(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="encodeTwoUints", args=[10, 20]))
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert len(result) == 64
        assert int.from_bytes(result[:32], "big") == 10
        assert int.from_bytes(result[32:], "big") == 20


class TestHashPacked:
    def test_deterministic(self, app, account):
        r1 = app.send.call(au.AppClientMethodCallParams(method="hashPacked", args=[account.address, 100]))
        r2 = app.send.call(au.AppClientMethodCallParams(method="hashPacked", args=[account.address, 100]))
        assert r1.abi_return == r2.abi_return

    def test_different_inputs(self, app, account):
        r1 = app.send.call(au.AppClientMethodCallParams(method="hashPacked", args=[account.address, 1]))
        r2 = app.send.call(au.AppClientMethodCallParams(method="hashPacked", args=[account.address, 2]))
        assert r1.abi_return != r2.abi_return


class TestMultipleReturns:
    def test_multi_return(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="multiReturn"))
        ret = r.abi_return
        if isinstance(ret, dict):
            vals = list(ret.values())
        else:
            vals = list(ret)
        assert vals[0] == 42
        assert vals[1] is True

    def test_named_tuple_return(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="tupleReturn"))
        ret = r.abi_return
        if isinstance(ret, dict):
            assert ret.get("x", ret.get(0)) == 10
            assert ret.get("y", ret.get(1)) == 20
        else:
            assert ret[0] == 10
            assert ret[1] == 20
