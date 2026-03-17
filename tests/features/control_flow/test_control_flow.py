"""Regression tests for control flow: if/else, for, while, break, continue, ternary."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "ControlFlow.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["ControlFlow"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestIfElse:
    def test_high(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="ifElse", args=[150])).abi_return == 3

    def test_mid(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="ifElse", args=[75])).abi_return == 2

    def test_low(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="ifElse", args=[10])).abi_return == 1


class TestForLoop:
    def test_sum_0_to_9(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="forLoop", args=[10])).abi_return == 45

    def test_zero_iterations(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="forLoop", args=[0])).abi_return == 0


class TestWhileLoop:
    def test_sum(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="whileLoop", args=[5])).abi_return == 10


class TestBreakContinue:
    def test_break_at_5(self, app):
        # sum of 0+1+2+3+4 = 10
        assert app.send.call(au.AppClientMethodCallParams(method="forWithBreak", args=[100])).abi_return == 10

    def test_continue_odd_only(self, app):
        # odd numbers < 10: 1+3+5+7+9 = 25
        assert app.send.call(au.AppClientMethodCallParams(method="forWithContinue", args=[10])).abi_return == 25


class TestTernary:
    def test_true(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="ternary", args=[True])).abi_return == 42

    def test_false(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="ternary", args=[False])).abi_return == 99


class TestEarlyReturn:
    def test_zero(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="earlyReturn", args=[0])).abi_return == 0

    def test_nonzero(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="earlyReturn", args=[5])).abi_return == 10
