"""
ERC1155SupplyTest behavioral tests.
Tests supply tracking for multi-token contracts.
"""

import hashlib
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def balance_box_key(account_addr: str, token_id: int) -> bytes:
    """Box key for _balances mapping: keccak256(abi.encodePacked(account, id))."""
    from Crypto.Hash import keccak
    addr_bytes = encoding.decode_address(account_addr)
    id_bytes = token_id.to_bytes(64, "big")
    h = keccak.new(digest_bits=256)
    h.update(addr_bytes + id_bytes)
    key_hash = h.digest()
    return b"_balances" + hashlib.sha256(key_hash).digest()


def supply_box_key(token_id: int) -> bytes:
    return mapping_box_key("_totalSupply", token_id.to_bytes(64, "big"))


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


@pytest.fixture(scope="module")
def supply(localnet, account):
    return deploy_contract(localnet, account, "ERC1155SupplyTest")


@pytest.fixture(scope="module")
def init_token(supply, account):
    """Initialize boxes for token 1 and account."""
    app_id = supply.app_id
    sk = supply_box_key(1)
    bk = balance_box_key(account.address, 1)
    supply.send.call(
        au.AppClientMethodCallParams(
            method="initSupply",
            args=[1],
            box_references=[box_ref(app_id, sk)],
        )
    )
    supply.send.call(
        au.AppClientMethodCallParams(
            method="initBalance",
            args=[account.address, 1],
            box_references=[box_ref(app_id, bk)],
        )
    )
    return True


def test_deploy(supply):
    assert supply.app_id > 0


def test_total_supply_all_initially_zero(supply):
    result = supply.send.call(
        au.AppClientMethodCallParams(method="totalSupplyAll")
    )
    assert result.abi_return == 0


def test_mint_tokens(supply, account, init_token):
    app_id = supply.app_id
    sk = supply_box_key(1)
    bk = balance_box_key(account.address, 1)

    supply.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1, 100],
            box_references=[box_ref(app_id, bk), box_ref(app_id, sk)],
        )
    )

    result = supply.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[1],
            box_references=[box_ref(app_id, sk)],
        )
    )
    assert result.abi_return == 100


def test_balance_after_mint(supply, account):
    app_id = supply.app_id
    bk = balance_box_key(account.address, 1)
    result = supply.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(app_id, bk)],
        )
    )
    assert result.abi_return == 100


def test_total_supply_all_after_mint(supply):
    result = supply.send.call(
        au.AppClientMethodCallParams(method="totalSupplyAll")
    )
    assert result.abi_return == 100


def test_exists_true(supply):
    app_id = supply.app_id
    sk = supply_box_key(1)
    result = supply.send.call(
        au.AppClientMethodCallParams(
            method="exists",
            args=[1],
            box_references=[box_ref(app_id, sk)],
        )
    )
    assert result.abi_return is True


def test_mint_more(supply, account):
    app_id = supply.app_id
    sk = supply_box_key(1)
    bk = balance_box_key(account.address, 1)
    supply.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1, 50],
            box_references=[box_ref(app_id, bk), box_ref(app_id, sk)],
        )
    )
    result = supply.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[1],
            box_references=[box_ref(app_id, sk)],
        )
    )
    assert result.abi_return == 150


def test_burn_tokens(supply, account):
    app_id = supply.app_id
    sk = supply_box_key(1)
    bk = balance_box_key(account.address, 1)
    supply.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 1, 30],
            box_references=[box_ref(app_id, bk), box_ref(app_id, sk)],
        )
    )
    result = supply.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(app_id, bk)],
        )
    )
    assert result.abi_return == 120


def test_total_supply_after_burn(supply):
    app_id = supply.app_id
    sk = supply_box_key(1)
    result = supply.send.call(
        au.AppClientMethodCallParams(
            method="totalSupply",
            args=[1],
            box_references=[box_ref(app_id, sk)],
        )
    )
    assert result.abi_return == 120


def test_burn_insufficient_fails(supply, account):
    app_id = supply.app_id
    sk = supply_box_key(1)
    bk = balance_box_key(account.address, 1)
    with pytest.raises(Exception):
        supply.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[account.address, 1, 999],
                box_references=[box_ref(app_id, bk), box_ref(app_id, sk)],
            )
        )
