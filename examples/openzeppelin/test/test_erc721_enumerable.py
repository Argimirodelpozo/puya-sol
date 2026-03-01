"""
ERC721Enumerable behavioral tests.

Tests the real OpenZeppelin v5.0.0 ERC721Enumerable (with minimal AVM
adaptations) for semantic correctness: totalSupply, tokenByIndex,
tokenOfOwnerByIndex, swap-and-pop removal, transfer enumeration updates.

AVM constraint: each transaction can reference at most 8 boxes. Operations
that require >8 box references (e.g., transferring/burning a token that is
NOT the last in the owner's enumeration, requiring swap in both owner and
global enumerations) need grouped transactions for resource pooling.
Tests here cover operations within the 8-box limit.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, fund_account


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def token_id_bytes(tid: int) -> bytes:
    return tid.to_bytes(64, "big")


def index_bytes(idx: int) -> bytes:
    return idx.to_bytes(64, "big")


# --- Box key helpers ---


def owners_box(token_id: int) -> bytes:
    return mapping_box_key("_owners", token_id_bytes(token_id))


def balances_box(addr: str) -> bytes:
    return mapping_box_key("_balances", addr_bytes(addr))


def token_approvals_box(token_id: int) -> bytes:
    return mapping_box_key("_tokenApprovals", token_id_bytes(token_id))


def operator_approvals_box(owner: str, operator: str) -> bytes:
    return mapping_box_key(
        "_operatorApprovals", addr_bytes(owner), addr_bytes(operator),
    )


def owned_tokens_box(owner: str, index: int) -> bytes:
    return mapping_box_key("_ownedTokens", addr_bytes(owner), index_bytes(index))


def owned_tokens_index_box(token_id: int) -> bytes:
    return mapping_box_key("_ownedTokensIndex", token_id_bytes(token_id))


def all_tokens_box(index: int) -> bytes:
    return mapping_box_key("_allTokens", index_bytes(index))


def all_tokens_index_box(token_id: int) -> bytes:
    return mapping_box_key("_allTokensIndex", token_id_bytes(token_id))


# --- Deploy and mint helpers ---


def deploy_nft(localnet, account):
    return deploy_contract(localnet, account, "ERC721EnumerableTest")


def mint_token(client, to_addr: str, token_id: int, global_index: int, owner_index: int):
    """Mint a token. 6 box references needed.

    global_index: _allTokensCount before this mint (0 for first token globally)
    owner_index: balanceOf(to) before this mint (0 for first token of this owner)
    """
    app_id = client.app_id
    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[to_addr, token_id],
            box_references=[
                box_ref(app_id, owners_box(token_id)),
                box_ref(app_id, balances_box(to_addr)),
                box_ref(app_id, all_tokens_box(global_index)),
                box_ref(app_id, all_tokens_index_box(token_id)),
                box_ref(app_id, owned_tokens_box(to_addr, owner_index)),
                box_ref(app_id, owned_tokens_index_box(token_id)),
            ],
        )
    )


# --- Deployment and basic tests ---


def test_deploy(localnet, account):
    client = deploy_nft(localnet, account)
    assert client.app_id > 0


def test_name(localnet, account):
    client = deploy_nft(localnet, account)
    result = client.send.call(au.AppClientMethodCallParams(method="name"))
    assert result.abi_return == "EnumerableNFT"


def test_symbol(localnet, account):
    client = deploy_nft(localnet, account)
    result = client.send.call(au.AppClientMethodCallParams(method="symbol"))
    assert result.abi_return == "ENFT"


def test_initial_total_supply(localnet, account):
    client = deploy_nft(localnet, account)
    result = client.send.call(au.AppClientMethodCallParams(method="totalSupply"))
    assert result.abi_return == 0


# --- supportsInterface ---


def test_supports_erc165(localnet, account):
    client = deploy_nft(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="supportsInterface",
            args=[bytes.fromhex("01ffc9a7")],
        )
    )
    assert result.abi_return is True


def test_supports_erc721(localnet, account):
    client = deploy_nft(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="supportsInterface",
            args=[bytes.fromhex("80ac58cd")],
        )
    )
    assert result.abi_return is True


def test_supports_erc721_enumerable(localnet, account):
    """ERC721Enumerable interfaceId = 0x780e9d63."""
    client = deploy_nft(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="supportsInterface",
            args=[bytes.fromhex("780e9d63")],
        )
    )
    assert result.abi_return is True


# --- Mint and global enumeration ---


def test_mint_total_supply(localnet, account):
    client = deploy_nft(localnet, account)
    mint_token(client, account.address, 1, 0, 0)
    result = client.send.call(au.AppClientMethodCallParams(method="totalSupply"))
    assert result.abi_return == 1


def test_mint_token_by_index(localnet, account):
    client = deploy_nft(localnet, account)
    mint_token(client, account.address, 42, 0, 0)

    app_id = client.app_id
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenByIndex",
            args=[0],
            box_references=[box_ref(app_id, all_tokens_box(0))],
        )
    )
    assert result.abi_return == 42


def test_mint_two_tokens_enumeration(localnet, account):
    """Mint two tokens and verify both global and owner enumeration."""
    client = deploy_nft(localnet, account)
    app_id = client.app_id

    mint_token(client, account.address, 10, 0, 0)
    mint_token(client, account.address, 20, 1, 1)

    # totalSupply = 2
    result = client.send.call(au.AppClientMethodCallParams(method="totalSupply"))
    assert result.abi_return == 2

    # tokenByIndex
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenByIndex", args=[0],
            box_references=[box_ref(app_id, all_tokens_box(0))],
        )
    )
    assert result.abi_return == 10

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenByIndex", args=[1],
            box_references=[box_ref(app_id, all_tokens_box(1))],
        )
    )
    assert result.abi_return == 20

    # tokenOfOwnerByIndex
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenOfOwnerByIndex",
            args=[account.address, 0],
            box_references=[
                box_ref(app_id, balances_box(account.address)),
                box_ref(app_id, owned_tokens_box(account.address, 0)),
            ],
        )
    )
    assert result.abi_return == 10

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenOfOwnerByIndex",
            args=[account.address, 1],
            box_references=[
                box_ref(app_id, balances_box(account.address)),
                box_ref(app_id, owned_tokens_box(account.address, 1)),
            ],
        )
    )
    assert result.abi_return == 20


# --- Multi-owner enumeration ---


def test_multi_owner_enumeration(localnet, account):
    """Mint to two different owners and verify per-owner enumeration."""
    client = deploy_nft(localnet, account)
    app_id = client.app_id

    account2 = localnet.account.random()
    fund_account(localnet, account, account2)

    mint_token(client, account.address, 1, 0, 0)
    mint_token(client, account2.address, 2, 1, 0)

    # Owner A has token 1 at index 0
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenOfOwnerByIndex",
            args=[account.address, 0],
            box_references=[
                box_ref(app_id, balances_box(account.address)),
                box_ref(app_id, owned_tokens_box(account.address, 0)),
            ],
        )
    )
    assert result.abi_return == 1

    # Owner B has token 2 at index 0
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenOfOwnerByIndex",
            args=[account2.address, 0],
            box_references=[
                box_ref(app_id, balances_box(account2.address)),
                box_ref(app_id, owned_tokens_box(account2.address, 0)),
            ],
        )
    )
    assert result.abi_return == 2


# --- Transfer ---


def test_transfer_updates_enumeration(localnet, account):
    """Transfer the only token from A to B (last in set, no swap needed).
    Requires 8 box references — exactly at the AVM limit."""
    client = deploy_nft(localnet, account)
    app_id = client.app_id

    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)

    mint_token(client, account.address, 1, 0, 0)

    # Transfer token 1 from account to to_addr (8 boxes)
    client.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, to_addr, 1],
            box_references=[
                box_ref(app_id, owners_box(1)),
                box_ref(app_id, token_approvals_box(1)),
                box_ref(app_id, operator_approvals_box(account.address, account.address)),
                box_ref(app_id, balances_box(account.address)),
                box_ref(app_id, balances_box(to_addr)),
                box_ref(app_id, owned_tokens_index_box(1)),
                box_ref(app_id, owned_tokens_box(account.address, 0)),
                box_ref(app_id, owned_tokens_box(to_addr, 0)),
            ],
        )
    )

    # Verify owner changed
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf", args=[1],
            box_references=[box_ref(app_id, owners_box(1))],
        )
    )
    assert result.abi_return == to_addr

    # Verify tokenOfOwnerByIndex for new owner
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenOfOwnerByIndex",
            args=[to_addr, 0],
            box_references=[
                box_ref(app_id, balances_box(to_addr)),
                box_ref(app_id, owned_tokens_box(to_addr, 0)),
            ],
        )
    )
    assert result.abi_return == 1

    # totalSupply unchanged
    result = client.send.call(au.AppClientMethodCallParams(method="totalSupply"))
    assert result.abi_return == 1


# --- Burn ---


def test_burn_last_token(localnet, account):
    """Mint two tokens, burn the last one (no swap needed). 7 box references."""
    client = deploy_nft(localnet, account)
    app_id = client.app_id

    mint_token(client, account.address, 1, 0, 0)
    mint_token(client, account.address, 2, 1, 1)

    # Burn token 2 (last in both owner and global enumerations)
    client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[2],
            box_references=[
                box_ref(app_id, owners_box(2)),
                box_ref(app_id, token_approvals_box(2)),
                box_ref(app_id, balances_box(account.address)),
                box_ref(app_id, owned_tokens_index_box(2)),
                box_ref(app_id, owned_tokens_box(account.address, 1)),
                box_ref(app_id, all_tokens_index_box(2)),
                box_ref(app_id, all_tokens_box(1)),
            ],
        )
    )

    # totalSupply = 1
    result = client.send.call(au.AppClientMethodCallParams(method="totalSupply"))
    assert result.abi_return == 1

    # tokenByIndex(0) = 1 (still there)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenByIndex", args=[0],
            box_references=[box_ref(app_id, all_tokens_box(0))],
        )
    )
    assert result.abi_return == 1

    # tokenOfOwnerByIndex(owner, 0) = 1
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenOfOwnerByIndex",
            args=[account.address, 0],
            box_references=[
                box_ref(app_id, balances_box(account.address)),
                box_ref(app_id, owned_tokens_box(account.address, 0)),
            ],
        )
    )
    assert result.abi_return == 1


# --- Out of bounds ---


def test_token_by_index_out_of_bounds(localnet, account):
    client = deploy_nft(localnet, account)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="tokenByIndex",
                args=[999],
                box_references=[box_ref(client.app_id, all_tokens_box(999))],
            )
        )


def test_token_of_owner_by_index_out_of_bounds(localnet, account):
    client = deploy_nft(localnet, account)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="tokenOfOwnerByIndex",
                args=[account.address, 999],
                box_references=[
                    box_ref(client.app_id, balances_box(account.address)),
                    box_ref(client.app_id, owned_tokens_box(account.address, 999)),
                ],
            )
        )


# --- Balance and owner queries ---


def test_balance_of(localnet, account):
    client = deploy_nft(localnet, account)
    app_id = client.app_id

    mint_token(client, account.address, 1, 0, 0)
    mint_token(client, account.address, 2, 1, 1)

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(app_id, balances_box(account.address))],
        )
    )
    assert result.abi_return == 2


def test_owner_of(localnet, account):
    client = deploy_nft(localnet, account)
    app_id = client.app_id

    mint_token(client, account.address, 1, 0, 0)

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf", args=[1],
            box_references=[box_ref(app_id, owners_box(1))],
        )
    )
    assert result.abi_return == account.address
