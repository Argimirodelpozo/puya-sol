"""Regression tests for Solidity mappings (single and nested)."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, mapping_box_key, box_ref, addr_to_bytes32, NO_POPULATE

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "Mappings.sol"
OUT_DIR = FEATURE_DIR / "out"

pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture
def app(compiled, localnet, account):
    c = compiled["Mappings"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


def bal_box(app_id, addr):
    return box_ref(app_id, mapping_box_key("balances", addr_to_bytes32(addr)))


class TestSingleMapping:
    def test_set_and_get(self, app, account):
        bx = bal_box(app.app_id, account.address)
        app.send.call(au.AppClientMethodCallParams(
            method="setBalance", args=[account.address, 1000],
            box_references=[bx],
        ))
        r = app.send.call(au.AppClientMethodCallParams(
            method="getBalance", args=[account.address],
            box_references=[bx],
        ))
        assert r.abi_return == 1000

    def test_compound_increment(self, app, account):
        bx = bal_box(app.app_id, account.address)
        app.send.call(au.AppClientMethodCallParams(
            method="setBalance", args=[account.address, 100],
            box_references=[bx],
        ))
        app.send.call(au.AppClientMethodCallParams(
            method="incrementBalance", args=[account.address, 50],
            box_references=[bx],
        ))
        r = app.send.call(au.AppClientMethodCallParams(
            method="getBalance", args=[account.address],
            box_references=[bx],
        ))
        assert r.abi_return == 150


class TestNestedMapping:
    def test_set_and_get(self, app, account):
        spender = app.app_address
        bx = box_ref(app.app_id, mapping_box_key(
            "allowances",
            addr_to_bytes32(account.address),
            addr_to_bytes32(spender),
        ))
        app.send.call(au.AppClientMethodCallParams(
            method="setAllowance", args=[account.address, spender, 500],
            box_references=[bx],
        ))
        r = app.send.call(au.AppClientMethodCallParams(
            method="getAllowance", args=[account.address, spender],
            box_references=[bx],
        ))
        assert r.abi_return == 500


class TestBoolMapping:
    def test_flag(self, app):
        bx = box_ref(app.app_id, mapping_box_key("flags", (42).to_bytes(32, "big")))
        app.send.call(au.AppClientMethodCallParams(
            method="setFlag", args=[42, True],
            box_references=[bx],
        ))
        r = app.send.call(au.AppClientMethodCallParams(
            method="getFlag", args=[42],
            box_references=[bx],
        ))
        assert r.abi_return is True
