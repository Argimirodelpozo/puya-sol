"""Regression tests for libraries and using-for directive."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Libraries.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Libraries"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestUsingFor:
    def test_add(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="testAdd", args=[3, 7])).abi_return == 10

    def test_mul(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="testMul", args=[6, 7])).abi_return == 42

    def test_clamp_low(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="testClamp", args=[1, 5, 10])).abi_return == 5

    def test_clamp_mid(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="testClamp", args=[7, 5, 10])).abi_return == 7

    def test_clamp_high(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="testClamp", args=[20, 5, 10])).abi_return == 10


class TestDirectLibraryCall:
    def test_direct(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="testDirect", args=[100, 200])).abi_return == 300
