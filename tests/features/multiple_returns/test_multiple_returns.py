"""Regression tests for functions returning multiple values."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "MultipleReturns.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["MultipleReturns"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


def _vals(ret):
    """Extract values from return (may be dict, tuple, or list)."""
    if isinstance(ret, dict):
        return list(ret.values())
    return list(ret)


class TestMultipleReturns:
    def test_two_values(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="twoValues"))
        vals = _vals(r.abi_return)
        assert vals[0] == 42
        assert vals[1] is True

    def test_three_values(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="threeValues"))
        vals = _vals(r.abi_return)
        assert vals == [1, 2, 3]

    def test_named_returns(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="namedReturns"))
        vals = _vals(r.abi_return)
        assert vals[0] == 10
        assert vals[1] == 20

    def test_conditional_true(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="conditionalReturn", args=[True]))
        vals = _vals(r.abi_return)
        assert vals[0] == 100
        assert vals[1] is True

    def test_conditional_false(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="conditionalReturn", args=[False]))
        vals = _vals(r.abi_return)
        assert vals[0] == 0
        assert vals[1] is False

    def test_computation(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="returnWithComputation", args=[3, 7]))
        vals = _vals(r.abi_return)
        assert vals[0] == 10  # sum
        assert vals[1] == 21  # product

    def test_use_internal(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="useInternal", args=[5]))
        assert r.abi_return == 25  # 5*2 + 5*3 = 10 + 15
