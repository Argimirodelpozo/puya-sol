"""Regression tests for fixed and dynamic arrays."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Arrays.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Arrays"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestFixedArrays:
    def test_sum(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="sumFixedArray")).abi_return == 15

    def test_get_element(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="getElement", args=[0])).abi_return == 10
        assert app.send.call(au.AppClientMethodCallParams(method="getElement", args=[2])).abi_return == 30

    def test_set_element(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="setElement")).abi_return == 42

    def test_length(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="fixedLength")).abi_return == 7


class TestDynamicArrays:
    def test_create_and_access(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="createDynamic", args=[5])).abi_return == 40


class TestReturnArray:
    def test_return_fixed(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="returnArray"))
        result = list(r.abi_return) if not isinstance(r.abi_return, list) else r.abi_return
        assert result == [100, 200, 300]
