"""
OpenZeppelin ERC721Royalty behavioral tests.
Tests ERC721 + ERC2981 combined: mint, royalty, burn.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


ZERO_ADDR = encoding.encode_address(b"\x00" * 32)


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def token_id_bytes(tid: int) -> bytes:
    return tid.to_bytes(64, "big")


@pytest.fixture(scope="module")
def erc721_royalty(localnet, account):
    return deploy_contract(localnet, account, "ERC721RoyaltyTest")


def test_deploy(erc721_royalty):
    assert erc721_royalty.app_id > 0


def test_name(erc721_royalty):
    result = erc721_royalty.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "RoyaltyNFT"


def test_symbol(erc721_royalty):
    result = erc721_royalty.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "RNFT"


def test_mint(erc721_royalty, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    erc721_royalty.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 1],
            box_references=[
                box_ref(erc721_royalty.app_id, owner_key),
                box_ref(erc721_royalty.app_id, balance_key),
            ],
        )
    )
    result = erc721_royalty.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf",
            args=[1],
            box_references=[box_ref(erc721_royalty.app_id, owner_key)],
        )
    )
    assert result.abi_return == account.address


def test_set_default_royalty(erc721_royalty, account):
    erc721_royalty.send.call(
        au.AppClientMethodCallParams(
            method="setDefaultRoyalty",
            args=[account.address, 500],  # 5%
        )
    )
    tid = token_id_bytes(1)
    rb = mapping_box_key("_tokenRoyaltyInfo", tid)
    result = erc721_royalty.send.call(
        au.AppClientMethodCallParams(
            method="royaltyInfo",
            args=[1, 10000],
            box_references=[box_ref(erc721_royalty.app_id, rb)],
        )
    )
    receiver, amount = result.abi_return
    assert receiver == account.address
    assert amount == 500


def test_set_token_specific_royalty(erc721_royalty, account):
    to_raw = b"\x02" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)
    tid = token_id_bytes(1)
    rb = mapping_box_key("_tokenRoyaltyInfo", tid)

    erc721_royalty.send.call(
        au.AppClientMethodCallParams(
            method="setTokenRoyalty",
            args=[1, to_addr, 1000],  # 10%
            box_references=[box_ref(erc721_royalty.app_id, rb)],
        )
    )
    result = erc721_royalty.send.call(
        au.AppClientMethodCallParams(
            method="royaltyInfo",
            args=[1, 10000],
            box_references=[box_ref(erc721_royalty.app_id, rb)],
        )
    )
    receiver, amount = result.abi_return
    assert receiver == to_addr
    assert amount == 1000


def test_burn(erc721_royalty, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    approval_key = mapping_box_key("_tokenApprovals", tid)
    erc721_royalty.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[1],
            box_references=[
                box_ref(erc721_royalty.app_id, owner_key),
                box_ref(erc721_royalty.app_id, balance_key),
                box_ref(erc721_royalty.app_id, approval_key),
            ],
        )
    )
    with pytest.raises(Exception):
        erc721_royalty.send.call(
            au.AppClientMethodCallParams(
                method="ownerOf",
                args=[1],
                box_references=[box_ref(erc721_royalty.app_id, owner_key)],
            )
        )
