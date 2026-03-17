"""Regression tests for ERC20 token pattern (the most common Solidity contract)."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, mapping_box_key, box_ref, addr_to_bytes32

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "ERC20Token.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture
def token(compiled, localnet, account):
    c = compiled["ERC20Token"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"],
                  constructor_args=[b"TestToken", b"TTK"])


def bal_box(app_id, addr):
    return box_ref(app_id, mapping_box_key("_balances", addr_to_bytes32(addr)))


def allow_box(app_id, owner, spender):
    return box_ref(app_id, mapping_box_key(
        "_allowances", addr_to_bytes32(owner), addr_to_bytes32(spender)))


class TestERC20Metadata:
    def test_name(self, token):
        assert token.send.call(au.AppClientMethodCallParams(method="name")).abi_return == "TestToken"

    def test_symbol(self, token):
        assert token.send.call(au.AppClientMethodCallParams(method="symbol")).abi_return == "TTK"

    def test_decimals(self, token):
        assert token.send.call(au.AppClientMethodCallParams(method="decimals")).abi_return == 18

    def test_initial_supply_zero(self, token):
        assert token.send.call(au.AppClientMethodCallParams(method="totalSupply")).abi_return == 0


class TestMint:
    def test_mint(self, token, account):
        bx = bal_box(token.app_id, account.address)
        token.send.call(au.AppClientMethodCallParams(
            method="mint", args=[account.address, 1000],
            box_references=[bx],
        ))
        r = token.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address],
            box_references=[bx],
        ))
        assert r.abi_return == 1000
        assert token.send.call(au.AppClientMethodCallParams(method="totalSupply")).abi_return == 1000


class TestTransfer:
    def test_transfer(self, token, account):
        sender_bx = bal_box(token.app_id, account.address)
        recip = token.app_address
        recip_bx = bal_box(token.app_id, recip)

        # Mint first
        token.send.call(au.AppClientMethodCallParams(
            method="mint", args=[account.address, 500],
            box_references=[sender_bx],
        ))
        # Transfer
        r = token.send.call(au.AppClientMethodCallParams(
            method="transfer", args=[recip, 200],
            box_references=[sender_bx, recip_bx],
        ))
        assert r.abi_return is True

        # Check balances
        assert token.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address],
            box_references=[sender_bx],
        )).abi_return == 300

        assert token.send.call(au.AppClientMethodCallParams(
            method="balanceOf", args=[recip],
            box_references=[recip_bx],
        )).abi_return == 200


class TestApproveAndTransferFrom:
    def test_approve(self, token, account):
        spender = token.app_address
        bx = allow_box(token.app_id, account.address, spender)
        r = token.send.call(au.AppClientMethodCallParams(
            method="approve", args=[spender, 1000],
            box_references=[bx],
        ))
        assert r.abi_return is True

        r = token.send.call(au.AppClientMethodCallParams(
            method="allowance", args=[account.address, spender],
            box_references=[bx],
        ))
        assert r.abi_return == 1000
