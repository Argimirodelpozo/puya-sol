"""
ERC1155URIStorage behavioral tests.
Tests per-token URI storage and base URI for multi-token standard.
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


def balance_box_key(token_id: int, account: str) -> bytes:
    """_balances[id][account] — nested mapping."""
    tid = token_id_bytes(token_id)
    addr = encoding.decode_address(account)
    return mapping_box_key("_balances", tid, addr)


def token_uri_box(tid_bytes: bytes) -> bytes:
    return mapping_box_key("_tokenURIs", tid_bytes)


@pytest.fixture(scope="module")
def token(localnet, account):
    return deploy_contract(localnet, account, "ERC1155URIStorageTest")


@pytest.fixture(scope="module")
def minted_token(token, account):
    """Mint 100 of token ID 1."""
    app_id = token.app_id
    bk = balance_box_key(1, account.address)
    token.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1, 100],
            box_references=[box_ref(app_id, bk)],
        )
    )
    return token


def test_deploy(token):
    assert token.app_id > 0


def test_default_uri(token):
    """Default uri() returns the constructor URI."""
    app_id = token.app_id
    tid = token_id_bytes(1)
    result = token.send.call(
        au.AppClientMethodCallParams(
            method="uri",
            args=[1],
            box_references=[box_ref(app_id, token_uri_box(tid))],
        )
    )
    assert result.abi_return == "https://example.com/{id}.json"


def test_balance_after_mint(minted_token, account):
    app_id = minted_token.app_id
    bk = balance_box_key(1, account.address)
    result = minted_token.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(app_id, bk)],
        )
    )
    assert result.abi_return == 100


def test_set_token_uri(minted_token):
    """Set a per-token URI for token 1."""
    app_id = minted_token.app_id
    tid = token_id_bytes(1)
    minted_token.send.call(
        au.AppClientMethodCallParams(
            method="setTokenURI",
            args=[1, "token1.json"],
            box_references=[box_ref(app_id, token_uri_box(tid))],
        )
    )


def test_uri_after_set(minted_token):
    """After setTokenURI, uri() returns the per-token URI (with empty base)."""
    app_id = minted_token.app_id
    tid = token_id_bytes(1)
    result = minted_token.send.call(
        au.AppClientMethodCallParams(
            method="uri",
            args=[1],
            box_references=[box_ref(app_id, token_uri_box(tid))],
        )
    )
    # With empty _baseURI, should return just the tokenURI
    assert result.abi_return == "token1.json"


def test_set_base_uri(minted_token):
    """Set a base URI."""
    minted_token.send.call(
        au.AppClientMethodCallParams(
            method="setBaseURI",
            args=["https://metadata.example.com/"],
        )
    )


def test_uri_with_base(minted_token):
    """After setBaseURI + setTokenURI, uri() returns baseURI + tokenURI."""
    app_id = minted_token.app_id
    tid = token_id_bytes(1)
    result = minted_token.send.call(
        au.AppClientMethodCallParams(
            method="uri",
            args=[1],
            box_references=[box_ref(app_id, token_uri_box(tid))],
        )
    )
    assert result.abi_return == "https://metadata.example.com/token1.json"


def test_uri_unset_token_falls_back(minted_token):
    """Token without per-token URI falls back to super.uri() (constructor URI)."""
    app_id = minted_token.app_id
    tid = token_id_bytes(999)
    result = minted_token.send.call(
        au.AppClientMethodCallParams(
            method="uri",
            args=[999],
            box_references=[box_ref(app_id, token_uri_box(tid))],
        )
    )
    # No per-token URI set for 999, should return constructor URI
    assert result.abi_return == "https://example.com/{id}.json"
