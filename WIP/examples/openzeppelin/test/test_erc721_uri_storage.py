"""
ERC721URIStorage behavioral tests.
Tests per-token URI storage for NFTs.
"""

import hashlib
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def token_id_bytes(tid: int) -> bytes:
    return tid.to_bytes(64, "big")


def owners_box(tid_bytes: bytes) -> bytes:
    return mapping_box_key("_owners", tid_bytes)


def balances_box(addr: str) -> bytes:
    return mapping_box_key("_balances", encoding.decode_address(addr))


def token_approvals_box(tid_bytes: bytes) -> bytes:
    return mapping_box_key("_tokenApprovals", tid_bytes)


def token_uri_box(tid_bytes: bytes) -> bytes:
    return mapping_box_key("_tokenURIs", tid_bytes)


@pytest.fixture(scope="module")
def nft(localnet, account):
    return deploy_contract(localnet, account, "ERC721URIStorageTest")


@pytest.fixture(scope="module")
def minted_nft(nft, account):
    """Mint token 1 and return the client."""
    app_id = nft.app_id
    tid = token_id_bytes(1)
    nft.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1],
            box_references=[
                box_ref(app_id, owners_box(tid)),
                box_ref(app_id, balances_box(account.address)),
                box_ref(app_id, token_approvals_box(tid)),
            ],
        )
    )
    return nft


def test_deploy(nft):
    assert nft.app_id > 0


def test_name(nft):
    result = nft.send.call(
        au.AppClientMethodCallParams(method="name", args=[])
    )
    assert result.abi_return == "URIStorageNFT"


def test_symbol(nft):
    result = nft.send.call(
        au.AppClientMethodCallParams(method="symbol", args=[])
    )
    assert result.abi_return == "USNFT"


def test_mint_and_owner(minted_nft, account):
    app_id = minted_nft.app_id
    tid = token_id_bytes(1)
    result = minted_nft.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf",
            args=[1],
            box_references=[box_ref(app_id, owners_box(tid))],
        )
    )
    assert result.abi_return == account.address


def test_token_uri_default_empty(minted_nft):
    """Before setTokenURI, tokenURI should return empty string."""
    app_id = minted_nft.app_id
    tid = token_id_bytes(1)
    result = minted_nft.send.call(
        au.AppClientMethodCallParams(
            method="tokenURI",
            args=[1],
            box_references=[
                box_ref(app_id, owners_box(tid)),
                box_ref(app_id, token_uri_box(tid)),
            ],
        )
    )
    assert result.abi_return == ""


def test_set_token_uri(minted_nft):
    """Set a URI for token 1."""
    app_id = minted_nft.app_id
    tid = token_id_bytes(1)
    minted_nft.send.call(
        au.AppClientMethodCallParams(
            method="setTokenURI",
            args=[1, "https://example.com/token/1"],
            box_references=[box_ref(app_id, token_uri_box(tid))],
        )
    )


def test_token_uri_after_set(minted_nft):
    """After setTokenURI, tokenURI should return the set value."""
    app_id = minted_nft.app_id
    tid = token_id_bytes(1)
    result = minted_nft.send.call(
        au.AppClientMethodCallParams(
            method="tokenURI",
            args=[1],
            box_references=[
                box_ref(app_id, owners_box(tid)),
                box_ref(app_id, token_uri_box(tid)),
            ],
        )
    )
    assert result.abi_return == "https://example.com/token/1"


def test_set_token_uri_update(minted_nft):
    """Update the URI for token 1."""
    app_id = minted_nft.app_id
    tid = token_id_bytes(1)
    minted_nft.send.call(
        au.AppClientMethodCallParams(
            method="setTokenURI",
            args=[1, "ipfs://QmNewHash"],
            box_references=[box_ref(app_id, token_uri_box(tid))],
            note=b"update_uri",
        )
    )
    result = minted_nft.send.call(
        au.AppClientMethodCallParams(
            method="tokenURI",
            args=[1],
            box_references=[
                box_ref(app_id, owners_box(tid)),
                box_ref(app_id, token_uri_box(tid)),
            ],
            note=b"read_updated_uri",
        )
    )
    assert result.abi_return == "ipfs://QmNewHash"


def test_balance_of(minted_nft, account):
    app_id = minted_nft.app_id
    result = minted_nft.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, balances_box(account.address))],
        )
    )
    assert result.abi_return == 1
