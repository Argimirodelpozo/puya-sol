"""
OpenZeppelin ERC2981 (Royalties) behavioral tests.
Tests default and per-token royalty settings.

NOTE: Reading from unset mapping keys (box doesn't exist in AVM) crashes,
so we only query royaltyInfo for tokens that have been explicitly set.
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


def royalty_box(token_id: int) -> bytes:
    """Box key for _tokenRoyaltyInfo[tokenId]."""
    tid = token_id.to_bytes(64, "big")
    return mapping_box_key("_tokenRoyaltyInfo", tid)


@pytest.fixture(scope="module")
def erc2981(localnet, account):
    return deploy_contract(localnet, account, "ERC2981Test")


def test_deploy(erc2981):
    assert erc2981.app_id > 0


def test_set_default_royalty(erc2981, account):
    """Set 5% royalty (500 basis points out of 10000)."""
    erc2981.send.call(
        au.AppClientMethodCallParams(
            method="setDefaultRoyalty",
            args=[account.address, 500],
        )
    )


def test_set_token_royalty(erc2981, account):
    """Set 10% royalty for token 42 and verify."""
    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)

    rb = royalty_box(42)
    erc2981.send.call(
        au.AppClientMethodCallParams(
            method="setTokenRoyalty",
            args=[42, to_addr, 1000],
            box_references=[box_ref(erc2981.app_id, rb)],
        )
    )
    result = erc2981.send.call(
        au.AppClientMethodCallParams(
            method="royaltyInfo",
            args=[42, 10000],
            box_references=[box_ref(erc2981.app_id, rb)],
        )
    )
    receiver, amount = result.abi_return
    assert receiver == to_addr
    assert amount == 1000  # 10% of 10000


def test_token_royalty_different_sale_price(erc2981):
    """Token 42 at different sale price."""
    rb = royalty_box(42)
    result = erc2981.send.call(
        au.AppClientMethodCallParams(
            method="royaltyInfo",
            args=[42, 50000],
            box_references=[box_ref(erc2981.app_id, rb)],
        )
    )
    _, amount = result.abi_return
    assert amount == 5000  # 10% of 50000


def test_reset_token_royalty(erc2981, account):
    """Reset token 42 royalty to re-set it."""
    rb = royalty_box(42)
    erc2981.send.call(
        au.AppClientMethodCallParams(
            method="resetTokenRoyalty",
            args=[42],
            box_references=[box_ref(erc2981.app_id, rb)],
        )
    )
    # After reset, re-set with a different percentage
    erc2981.send.call(
        au.AppClientMethodCallParams(
            method="setTokenRoyalty",
            args=[42, account.address, 200],  # 2%
            box_references=[box_ref(erc2981.app_id, rb)],
        )
    )
    result = erc2981.send.call(
        au.AppClientMethodCallParams(
            method="royaltyInfo",
            args=[42, 10000],
            box_references=[box_ref(erc2981.app_id, rb)],
        )
    )
    receiver, amount = result.abi_return
    assert receiver == account.address
    assert amount == 200  # 2% of 10000


def test_set_royalty_zero_receiver_fails(erc2981):
    with pytest.raises(Exception):
        erc2981.send.call(
            au.AppClientMethodCallParams(
                method="setDefaultRoyalty",
                args=[ZERO_ADDR, 500],
            )
        )


def test_set_royalty_exceeds_denominator_fails(erc2981, account):
    with pytest.raises(Exception):
        erc2981.send.call(
            au.AppClientMethodCallParams(
                method="setDefaultRoyalty",
                args=[account.address, 10001],  # > 10000
            )
        )
