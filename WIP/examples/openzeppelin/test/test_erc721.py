"""
OpenZeppelin ERC721 behavioral tests.

Tests the compiled OpenZeppelin ERC721 contract (exact v5.0.0 source) for
semantic correctness on AVM: minting, transfers, approvals, operator
approvals, burn, and error cases.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key

ZERO_ADDR = encoding.encode_address(b"\x00" * 32)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def owners_box(token_id_bytes: bytes) -> bytes:
    return mapping_box_key("_owners", token_id_bytes)


def balances_box(addr: str) -> bytes:
    return mapping_box_key("_balances", encoding.decode_address(addr))


def token_approvals_box(token_id_bytes: bytes) -> bytes:
    return mapping_box_key("_tokenApprovals", token_id_bytes)


def operator_approvals_box(owner: str, operator: str) -> bytes:
    return mapping_box_key(
        "_operatorApprovals",
        encoding.decode_address(owner),
        encoding.decode_address(operator),
    )


def token_id_bytes(tid: int) -> bytes:
    """Convert token ID to 64-byte big-endian biguint."""
    return tid.to_bytes(64, "big")


def deploy_erc721(localnet, account):
    """Deploy ERC721Test."""
    return deploy_contract(
        localnet, account, "ERC721Test",
    )


def mint_token(client, to_addr, token_id, app_id):
    """Mint a token and return the result."""
    tid = token_id_bytes(token_id)
    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[to_addr, token_id],
            box_references=[
                box_ref(app_id, owners_box(tid)),
                box_ref(app_id, balances_box(to_addr)),
                box_ref(app_id, token_approvals_box(tid)),
            ],
        )
    )


# --- Deployment tests ---


@pytest.mark.localnet
def test_deploys(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    assert client.app_id > 0


@pytest.mark.localnet
def test_name(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "TestNFT"


@pytest.mark.localnet
def test_symbol(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "TNFT"


# --- Mint tests ---


@pytest.mark.localnet
def test_mint(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    # Check ownerOf
    tid = token_id_bytes(1)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf",
            args=[1],
            box_references=[box_ref(client.app_id, owners_box(tid))],
        )
    )
    assert result.abi_return == account.address


@pytest.mark.localnet
def test_mint_increases_balance(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(client.app_id, balances_box(account.address))],
        )
    )
    assert result.abi_return == 1


@pytest.mark.localnet
def test_mint_to_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    tid = token_id_bytes(1)
    zero_raw = b"\x00" * 32
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[ZERO_ADDR, 1],
                box_references=[
                    box_ref(client.app_id, owners_box(tid)),
                    box_ref(client.app_id, mapping_box_key("_balances", zero_raw)),
                    box_ref(client.app_id, token_approvals_box(tid)),
                ],
            )
        )


@pytest.mark.localnet
def test_mint_duplicate_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    tid = token_id_bytes(1)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[account.address, 1],
                box_references=[
                    box_ref(client.app_id, owners_box(tid)),
                    box_ref(client.app_id, balances_box(account.address)),
                    box_ref(client.app_id, token_approvals_box(tid)),
                ],
            )
        )


# --- Transfer tests ---


@pytest.mark.localnet
def test_transfer_from(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    # Transfer to a known address
    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)
    tid = token_id_bytes(1)

    client.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, to_addr, 1],
            box_references=[
                box_ref(client.app_id, owners_box(tid)),
                box_ref(client.app_id, balances_box(account.address)),
                box_ref(client.app_id, balances_box(to_addr)),
                box_ref(client.app_id, token_approvals_box(tid)),
                box_ref(client.app_id, operator_approvals_box(account.address, account.address)),
            ],
        )
    )

    # Check new owner
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf",
            args=[1],
            box_references=[box_ref(client.app_id, owners_box(tid))],
        )
    )
    assert result.abi_return == to_addr


@pytest.mark.localnet
def test_transfer_to_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    tid = token_id_bytes(1)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="transferFrom",
                args=[account.address, ZERO_ADDR, 1],
                box_references=[
                    box_ref(client.app_id, owners_box(tid)),
                    box_ref(client.app_id, balances_box(account.address)),
                    box_ref(client.app_id, token_approvals_box(tid)),
                ],
            )
        )


# --- Approval tests ---


@pytest.mark.localnet
def test_approve(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)
    tid = token_id_bytes(1)

    client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[to_addr, 1],
            box_references=[
                box_ref(client.app_id, owners_box(tid)),
                box_ref(client.app_id, token_approvals_box(tid)),
                box_ref(client.app_id, operator_approvals_box(account.address, account.address)),
            ],
        )
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getApproved",
            args=[1],
            box_references=[
                box_ref(client.app_id, owners_box(tid)),
                box_ref(client.app_id, token_approvals_box(tid)),
            ],
        )
    )
    assert result.abi_return == to_addr


@pytest.mark.localnet
def test_set_approval_for_all(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)

    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)

    client.send.call(
        au.AppClientMethodCallParams(
            method="setApprovalForAll",
            args=[to_addr, True],
            box_references=[
                box_ref(client.app_id, operator_approvals_box(account.address, to_addr)),
            ],
        )
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="isApprovedForAll",
            args=[account.address, to_addr],
            box_references=[
                box_ref(client.app_id, operator_approvals_box(account.address, to_addr)),
            ],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_set_operator_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="setApprovalForAll",
                args=[ZERO_ADDR, True],
                box_references=[
                    box_ref(client.app_id, operator_approvals_box(account.address, ZERO_ADDR)),
                ],
            )
        )


# --- Burn tests ---


@pytest.mark.localnet
def test_burn(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    tid = token_id_bytes(1)
    client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[1],
            box_references=[
                box_ref(client.app_id, owners_box(tid)),
                box_ref(client.app_id, balances_box(account.address)),
                box_ref(client.app_id, token_approvals_box(tid)),
            ],
        )
    )

    # ownerOf should now fail (token doesn't exist)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="ownerOf",
                args=[1],
                box_references=[box_ref(client.app_id, owners_box(tid))],
            )
        )


@pytest.mark.localnet
def test_burn_nonexistent_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    tid = token_id_bytes(999)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[999],
                box_references=[
                    box_ref(client.app_id, owners_box(tid)),
                    box_ref(client.app_id, balances_box(account.address)),
                    box_ref(client.app_id, token_approvals_box(tid)),
                ],
            )
        )


# --- Balance tests ---


@pytest.mark.localnet
def test_balance_of_zero_address_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc721(localnet, account)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="balanceOf",
                args=[ZERO_ADDR],
                box_references=[
                    box_ref(client.app_id, balances_box(ZERO_ADDR)),
                ],
            )
        )


@pytest.mark.localnet
def test_token_uri(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """tokenURI returns empty string (test wrapper overrides base)."""
    client = deploy_erc721(localnet, account)
    mint_token(client, account.address, 1, client.app_id)

    tid = token_id_bytes(1)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="tokenURI",
            args=[1],
            box_references=[box_ref(client.app_id, owners_box(tid))],
        )
    )
    assert result.abi_return == ""
