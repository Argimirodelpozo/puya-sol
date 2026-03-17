"""Regression tests for error handling: require, assert, revert, custom errors."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, NO_POPULATE

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Errors.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["Errors"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestRequire:
    def test_pass(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="requirePass", args=[5]))
        assert r.abi_return == 5

    def test_fail(self, app):
        with pytest.raises(Exception, match="always fails|must be positive|assert"):
            app.send.call(
                au.AppClientMethodCallParams(method="requireFail"),
                send_params=NO_POPULATE,
            )


class TestAssert:
    def test_pass(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="assertPass"))
        assert r.abi_return is True


class TestRevert:
    def test_explicit_revert(self, app):
        with pytest.raises(Exception, match="explicit revert|assert"):
            app.send.call(
                au.AppClientMethodCallParams(method="revertAlways"),
                send_params=NO_POPULATE,
            )

    def test_custom_error(self, app):
        with pytest.raises(Exception, match="InsufficientBalance|assert"):
            app.send.call(
                au.AppClientMethodCallParams(
                    method="revertCustomError", args=[10, 100],
                ),
                send_params=NO_POPULATE,
            )

    def test_custom_error_no_revert(self, app):
        # Should NOT revert when balance >= needed
        app.send.call(au.AppClientMethodCallParams(
            method="revertCustomError", args=[100, 10],
        ))
