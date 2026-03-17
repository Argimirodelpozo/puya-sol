"""Regression tests for Solidity modifiers including epilog (lock pattern)."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, NO_POPULATE

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Modifiers.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture
def app(compiled, localnet, account):
    c = compiled["Modifiers"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestOnlyOwner:
    def test_owner_can_set(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setValue", args=[42]))
        r = app.send.call(au.AppClientMethodCallParams(method="value"))
        assert r.abi_return == 42


class TestWhenNotPaused:
    def test_set_when_not_paused(self, app):
        app.send.call(au.AppClientMethodCallParams(method="setValue", args=[10]))
        assert app.send.call(au.AppClientMethodCallParams(method="value")).abi_return == 10

    def test_revert_when_paused(self, app):
        app.send.call(au.AppClientMethodCallParams(method="pause"))
        with pytest.raises(Exception, match="paused|assert"):
            app.send.call(
                au.AppClientMethodCallParams(method="setValue", args=[99]),
                send_params=NO_POPULATE,
            )


class TestLockModifier:
    def test_lock_and_unlock(self, app):
        """Lock modifier should set unlocked=0 during execution, then restore to 1."""
        r = app.send.call(au.AppClientMethodCallParams(method="lockedIncrement"))
        assert r.abi_return == 1
        # After execution, unlocked should be 1 again
        assert app.send.call(au.AppClientMethodCallParams(method="getUnlocked")).abi_return == 1

    def test_second_call_works(self, app):
        """Second call should also work (lock released after first)."""
        app.send.call(au.AppClientMethodCallParams(method="lockedIncrement"))
        r = app.send.call(au.AppClientMethodCallParams(method="lockedIncrement"))
        assert r.abi_return == 2
