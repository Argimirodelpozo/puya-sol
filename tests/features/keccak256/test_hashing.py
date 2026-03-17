"""Regression tests for keccak256 and sha256 hashing."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Hashing.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Hashing"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestKeccak256:
    def test_hash_uint(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="hashUint", args=[42]))
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert len(result) == 32
        assert result != b"\x00" * 32

    def test_deterministic(self, app):
        r1 = app.send.call(au.AppClientMethodCallParams(method="hashUint", args=[100]))
        r2 = app.send.call(au.AppClientMethodCallParams(method="hashUint", args=[100]))
        assert r1.abi_return == r2.abi_return

    def test_different_inputs_different_hash(self, app):
        r1 = app.send.call(au.AppClientMethodCallParams(method="hashUint", args=[1]))
        r2 = app.send.call(au.AppClientMethodCallParams(method="hashUint", args=[2]))
        assert r1.abi_return != r2.abi_return

    def test_hash_packed_two_values(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="hashTwoValues", args=[10, 20]))
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert len(result) == 32

    def test_double_hash_determinism(self, app):
        """doubleHash checks h1 == h2 internally, reverts if not."""
        r = app.send.call(au.AppClientMethodCallParams(method="doubleHash", args=[999]))
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert len(result) == 32


class TestSha256:
    def test_sha256_hash(self, app):
        data = b"hello"
        r = app.send.call(au.AppClientMethodCallParams(method="sha256Hash", args=[data]))
        result = r.abi_return if isinstance(r.abi_return, bytes) else bytes(r.abi_return)
        assert len(result) == 32

        # Verify against known SHA-256
        import hashlib
        expected = hashlib.sha256(b"hello").digest()
        assert result == expected
