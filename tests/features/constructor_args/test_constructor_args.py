"""Regression tests for constructor parameters passed at deploy time."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "ConstructorArgs.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def app(compiled, localnet, account):
    c = compiled["ConstructorArgs"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"],
                  constructor_args=[b"TestContract", (42).to_bytes(32, "big")])


class TestConstructorArgs:
    def test_name_set(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="getName"))
        assert r.abi_return == "TestContract"

    def test_initial_value_set(self, app):
        r = app.send.call(au.AppClientMethodCallParams(method="getInitialValue"))
        assert r.abi_return == 42

    def test_owner_is_deployer(self, app, account):
        r = app.send.call(au.AppClientMethodCallParams(method="getOwner"))
        assert r.abi_return == account.address
