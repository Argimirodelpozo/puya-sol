"""
OpenZeppelin ERC1155 behavioral tests.

Tests the compiled OpenZeppelin ERC1155 contract (exact v5.0.0 source) for
semantic correctness on AVM: minting, transfers, approvals, burn, URI,
and error cases.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key

ZERO_ADDR = encoding.encode_address(b"\x00" * 32)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def balances_box(token_id: int, addr: str) -> bytes:
    """Box key for _balances[id][account]."""
    tid = token_id.to_bytes(64, "big")
    raw_addr = encoding.decode_address(addr)
    return mapping_box_key("_balances", tid, raw_addr)


def operator_box(owner: str, operator: str) -> bytes:
    return mapping_box_key(
        "_operatorApprovals",
        encoding.decode_address(owner),
        encoding.decode_address(operator),
    )


def deploy_erc1155(localnet, account):
    return deploy_contract(
        localnet, account, "ERC1155Test",
    )


def mint_token(client, to_addr, token_id, amount, app_id):
    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[to_addr, token_id, amount],
            box_references=[
                box_ref(app_id, balances_box(token_id, to_addr)),
            ],
        )
    )


# --- Deployment tests ---


@pytest.mark.localnet
def test_deploys(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    assert client.app_id > 0


@pytest.mark.localnet
def test_uri(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(method="uri", args=[1])
    )
    assert result.abi_return == "https://example.com/{id}.json"


# --- Mint tests ---


@pytest.mark.localnet
def test_mint(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    mint_token(client, account.address, 1, 100, client.app_id)

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(client.app_id, balances_box(1, account.address))],
        )
    )
    assert result.abi_return == 100


@pytest.mark.localnet
def test_mint_multiple_ids(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    mint_token(client, account.address, 1, 50, client.app_id)
    mint_token(client, account.address, 2, 75, client.app_id)

    r1 = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(client.app_id, balances_box(1, account.address))],
        )
    )
    r2 = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 2],
            box_references=[box_ref(client.app_id, balances_box(2, account.address))],
        )
    )
    assert r1.abi_return == 50
    assert r2.abi_return == 75


@pytest.mark.localnet
def test_mint_to_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[ZERO_ADDR, 1, 100],
                box_references=[box_ref(client.app_id, balances_box(1, ZERO_ADDR))],
            )
        )


# --- Transfer tests ---


@pytest.mark.localnet
def test_safe_transfer_from(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    mint_token(client, account.address, 1, 100, client.app_id)

    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)

    client.send.call(
        au.AppClientMethodCallParams(
            method="safeTransferFrom",
            args=[account.address, to_addr, 1, 40, b""],
            box_references=[
                box_ref(client.app_id, balances_box(1, account.address)),
                box_ref(client.app_id, balances_box(1, to_addr)),
                box_ref(client.app_id, operator_box(account.address, account.address)),
            ],
        )
    )

    r1 = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(client.app_id, balances_box(1, account.address))],
        )
    )
    r2 = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[to_addr, 1],
            box_references=[box_ref(client.app_id, balances_box(1, to_addr))],
        )
    )
    assert r1.abi_return == 60
    assert r2.abi_return == 40


@pytest.mark.localnet
def test_transfer_to_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    mint_token(client, account.address, 1, 100, client.app_id)

    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="safeTransferFrom",
                args=[account.address, ZERO_ADDR, 1, 10, b""],
                box_references=[
                    box_ref(client.app_id, balances_box(1, account.address)),
                    box_ref(client.app_id, balances_box(1, ZERO_ADDR)),
                ],
            )
        )


@pytest.mark.localnet
def test_transfer_insufficient_balance_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    mint_token(client, account.address, 1, 10, client.app_id)

    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)

    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="safeTransferFrom",
                args=[account.address, to_addr, 1, 999, b""],
                box_references=[
                    box_ref(client.app_id, balances_box(1, account.address)),
                    box_ref(client.app_id, balances_box(1, to_addr)),
                    box_ref(client.app_id, operator_box(account.address, account.address)),
                ],
            )
        )


# --- Approval tests ---


@pytest.mark.localnet
def test_set_approval_for_all(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    to_raw = b"\x01" + b"\x00" * 31
    to_addr = encoding.encode_address(to_raw)

    client.send.call(
        au.AppClientMethodCallParams(
            method="setApprovalForAll",
            args=[to_addr, True],
            box_references=[box_ref(client.app_id, operator_box(account.address, to_addr))],
        )
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="isApprovedForAll",
            args=[account.address, to_addr],
            box_references=[box_ref(client.app_id, operator_box(account.address, to_addr))],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_set_operator_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="setApprovalForAll",
                args=[ZERO_ADDR, True],
                box_references=[box_ref(client.app_id, operator_box(account.address, ZERO_ADDR))],
            )
        )


# --- Burn tests ---


@pytest.mark.localnet
def test_burn(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    mint_token(client, account.address, 1, 100, client.app_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 1, 30],
            box_references=[box_ref(client.app_id, balances_box(1, account.address))],
        )
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 1],
            box_references=[box_ref(client.app_id, balances_box(1, account.address))],
        )
    )
    assert result.abi_return == 70


@pytest.mark.localnet
def test_burn_from_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[ZERO_ADDR, 1, 10],
                box_references=[box_ref(client.app_id, balances_box(1, ZERO_ADDR))],
            )
        )


@pytest.mark.localnet
def test_burn_insufficient_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    mint_token(client, account.address, 1, 10, client.app_id)

    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[account.address, 1, 999],
                box_references=[box_ref(client.app_id, balances_box(1, account.address))],
            )
        )


# --- Balance default ---


@pytest.mark.localnet
def test_balance_default_zero(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_erc1155(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address, 999],
            box_references=[box_ref(client.app_id, balances_box(999, account.address))],
        )
    )
    assert result.abi_return == 0
