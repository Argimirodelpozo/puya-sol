"""Regression tests for user-defined value types (UDVT) with operator overloading."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, mapping_box_key, box_ref, addr_to_bytes32

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "UserDefinedTypes.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture
def app(compiled, localnet, account):
    c = compiled["UserDefinedTypes"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestWrapUnwrap:
    def test_identity(self, app):
        """wrap then unwrap should be identity."""
        assert app.send.call(au.AppClientMethodCallParams(method="wrapUnwrap", args=[42])).abi_return == 42

    def test_zero(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="wrapUnwrap", args=[0])).abi_return == 0

    def test_large(self, app):
        big = 10**50
        assert app.send.call(au.AppClientMethodCallParams(method="wrapUnwrap", args=[big])).abi_return == big


class TestUDVTUint64:
    def test_user_id(self, app):
        assert app.send.call(au.AppClientMethodCallParams(method="createUserId", args=[12345])).abi_return == 12345


class TestOperatorOverloading:
    def test_mint_and_get_balance(self, app, account):
        bal_box = box_ref(app.app_id, mapping_box_key("balances", addr_to_bytes32(account.address)))
        app.send.call(au.AppClientMethodCallParams(
            method="mint", args=[account.address, 1000],
            box_references=[bal_box],
        ))
        r = app.send.call(au.AppClientMethodCallParams(
            method="getBalance", args=[account.address],
            box_references=[bal_box],
        ))
        assert r.abi_return == 1000

    def test_mint_updates_total_supply(self, app, account):
        bal_box = box_ref(app.app_id, mapping_box_key("balances", addr_to_bytes32(account.address)))
        app.send.call(au.AppClientMethodCallParams(
            method="mint", args=[account.address, 500],
            box_references=[bal_box],
        ))
        assert app.send.call(au.AppClientMethodCallParams(method="getTotalSupply")).abi_return == 500

    def test_transfer(self, app, account):
        sender = account.address
        recipient = app.app_address
        sender_box = box_ref(app.app_id, mapping_box_key("balances", addr_to_bytes32(sender)))
        recip_box = box_ref(app.app_id, mapping_box_key("balances", addr_to_bytes32(recipient)))

        # Mint to sender
        app.send.call(au.AppClientMethodCallParams(
            method="mint", args=[sender, 1000],
            box_references=[sender_box],
        ))
        # Transfer
        app.send.call(au.AppClientMethodCallParams(
            method="transfer", args=[sender, recipient, 300],
            box_references=[sender_box, recip_box],
        ))
        # Check
        assert app.send.call(au.AppClientMethodCallParams(
            method="getBalance", args=[sender],
            box_references=[sender_box],
        )).abi_return == 700
        assert app.send.call(au.AppClientMethodCallParams(
            method="getBalance", args=[recipient],
            box_references=[recip_box],
        )).abi_return == 300


class TestPercentage:
    def test_apply(self, app):
        # 200 * 25% = 50
        assert app.send.call(au.AppClientMethodCallParams(
            method="applyPercentage", args=[200, 25]
        )).abi_return == 50
