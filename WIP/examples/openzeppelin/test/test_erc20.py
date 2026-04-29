"""
OpenZeppelin ERC20 behavioral tests.

Tests the compiled OpenZeppelin ERC20 contract (exact v5.0.0 source) for
semantic correctness on AVM: deployment, name/symbol/decimals, minting,
transfers, approvals, transferFrom, burn, and error cases.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key

INITIAL_SUPPLY = 1_000_000
ZERO_ADDR = encoding.encode_address(b"\x00" * 32)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


@pytest.fixture(scope="module")
def token_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy ERC20Test with __postInit and mint initial supply."""
    sender_raw = encoding.decode_address(account.address)
    balances_box_key = mapping_box_key("_balances", sender_raw)

    client = deploy_contract(
        localnet, account, "ERC20Test",
    )

    # Mint initial supply to deployer
    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, INITIAL_SUPPLY],
            box_references=[box_ref(client.app_id, balances_box_key)],
        )
    )
    return client


# --- Deployment tests ---


@pytest.mark.localnet
def test_deploys(token_client: au.AppClient) -> None:
    """Contract should deploy successfully."""
    assert token_client.app_id > 0


@pytest.mark.localnet
def test_name(token_client: au.AppClient) -> None:
    """name() should return 'TestToken'."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "TestToken"


@pytest.mark.localnet
def test_symbol(token_client: au.AppClient) -> None:
    """symbol() should return 'TT'."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "TT"


@pytest.mark.localnet
def test_decimals(token_client: au.AppClient) -> None:
    """decimals() should return 18."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="decimals")
    )
    assert result.abi_return == 18


# --- Supply and balance tests ---


@pytest.mark.localnet
def test_total_supply(token_client: au.AppClient) -> None:
    """totalSupply() should return the initial supply."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    assert result.abi_return == INITIAL_SUPPLY


@pytest.mark.localnet
def test_balance_of_deployer(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Deployer should have the full initial supply."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address]
        )
    )
    assert result.abi_return == INITIAL_SUPPLY


@pytest.mark.localnet
def test_balance_of_zero_address(token_client: au.AppClient) -> None:
    """Zero address should have zero balance."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf", args=[ZERO_ADDR]
        )
    )
    assert result.abi_return == 0


# --- Transfer tests ---


@pytest.mark.localnet
def test_transfer_to_self(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Self-transfer should succeed and not change balance."""
    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[account.address, 100],
            box_references=[box_ref(token_client.app_id, balances_box)],
        )
    )
    assert result.abi_return is True

    balance = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf", args=[account.address]
        )
    )
    assert balance.abi_return == INITIAL_SUPPLY


@pytest.mark.localnet
def test_transfer_zero(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Transferring 0 tokens should succeed."""
    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[account.address, 0],
            box_references=[box_ref(token_client.app_id, balances_box)],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_transfer_to_zero_address_fails(
    token_client: au.AppClient, account: SigningAccount,
) -> None:
    """Transfer to address(0) should revert (ERC20InvalidReceiver)."""
    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    zero_raw = b"\x00" * 32
    zero_box = mapping_box_key("_balances", zero_raw)
    with pytest.raises(Exception):
        token_client.send.call(
            au.AppClientMethodCallParams(
                method="transfer",
                args=[ZERO_ADDR, 100],
                box_references=[
                    box_ref(token_client.app_id, balances_box),
                    box_ref(token_client.app_id, zero_box),
                ],
            )
        )


# --- Approve tests ---


@pytest.mark.localnet
def test_approve(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """approve() should return true and set allowance."""
    sender_raw = encoding.decode_address(account.address)
    allowances_box = mapping_box_key(
        "_allowances", sender_raw, sender_raw
    )
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 500],
            box_references=[box_ref(token_client.app_id, allowances_box)],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_allowance_after_approve(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """allowance() should reflect the approved amount."""
    sender_raw = encoding.decode_address(account.address)
    allowances_box = mapping_box_key(
        "_allowances", sender_raw, sender_raw
    )
    # First approve
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 999],
            box_references=[box_ref(token_client.app_id, allowances_box)],
        )
    )
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, account.address],
            box_references=[box_ref(token_client.app_id, allowances_box)],
        )
    )
    assert result.abi_return == 999


@pytest.mark.localnet
def test_allowance_default_zero(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Default allowance for unset pairs should be 0."""
    zero_raw = b"\x00" * 32
    sender_raw = encoding.decode_address(account.address)
    allowances_box = mapping_box_key("_allowances", zero_raw, sender_raw)
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[ZERO_ADDR, account.address],
            box_references=[box_ref(token_client.app_id, allowances_box)],
        )
    )
    assert result.abi_return == 0


# --- TransferFrom tests ---


@pytest.mark.localnet
def test_transfer_from_with_allowance(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """transferFrom with sufficient allowance should succeed."""
    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    allowances_box = mapping_box_key(
        "_allowances", sender_raw, sender_raw
    )
    # Approve self to spend own tokens (for testing)
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 200],
            box_references=[box_ref(token_client.app_id, allowances_box)],
        )
    )
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, account.address, 100],
            box_references=[
                box_ref(token_client.app_id, balances_box),
                box_ref(token_client.app_id, allowances_box),
            ],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_transfer_from_zero_value(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """transferFrom with 0 value should work."""
    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    allowances_box = mapping_box_key(
        "_allowances", sender_raw, sender_raw
    )
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, account.address, 0],
            box_references=[
                box_ref(token_client.app_id, balances_box),
                box_ref(token_client.app_id, allowances_box),
            ],
        )
    )
    assert result.abi_return is True


# --- Mint and burn tests ---


@pytest.mark.localnet
def test_mint_increases_supply(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """mint() should increase total supply and recipient balance."""
    supply_before = token_client.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    ).abi_return

    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 500],
            box_references=[box_ref(token_client.app_id, balances_box)],
        )
    )
    supply_after = token_client.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    ).abi_return
    assert supply_after == supply_before + 500


@pytest.mark.localnet
def test_mint_to_zero_address_fails(token_client: au.AppClient) -> None:
    """Minting to address(0) should revert (ERC20InvalidReceiver)."""
    zero_raw = b"\x00" * 32
    zero_box = mapping_box_key("_balances", zero_raw)
    with pytest.raises(Exception):
        token_client.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[ZERO_ADDR, 100],
                box_references=[box_ref(token_client.app_id, zero_box)],
            )
        )


@pytest.mark.localnet
def test_burn(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """burn() should decrease balance and total supply."""
    supply_before = token_client.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    ).abi_return

    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[account.address, 100],
            box_references=[box_ref(token_client.app_id, balances_box)],
        )
    )

    supply_after = token_client.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    ).abi_return
    assert supply_after == supply_before - 100


@pytest.mark.localnet
def test_burn_from_zero_address_fails(token_client: au.AppClient) -> None:
    """Burning from address(0) should revert (ERC20InvalidSender)."""
    zero_raw = b"\x00" * 32
    zero_box = mapping_box_key("_balances", zero_raw)
    with pytest.raises(Exception):
        token_client.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[ZERO_ADDR, 100],
                box_references=[box_ref(token_client.app_id, zero_box)],
            )
        )


# --- Insufficient balance/allowance tests ---


@pytest.mark.localnet
def test_transfer_insufficient_balance_fails(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """Transfer more than balance should revert (ERC20InsufficientBalance)."""
    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    huge_amount = 10**30  # Way more than initial supply
    with pytest.raises(Exception):
        token_client.send.call(
            au.AppClientMethodCallParams(
                method="transfer",
                args=[account.address, huge_amount],
                box_references=[box_ref(token_client.app_id, balances_box)],
            )
        )


@pytest.mark.localnet
def test_transfer_from_insufficient_allowance_fails(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """transferFrom without sufficient allowance should revert."""
    sender_raw = encoding.decode_address(account.address)
    balances_box = mapping_box_key("_balances", sender_raw)
    allowances_box = mapping_box_key(
        "_allowances", sender_raw, sender_raw
    )
    # Set a small allowance
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 1],
            box_references=[box_ref(token_client.app_id, allowances_box)],
        )
    )
    with pytest.raises(Exception):
        token_client.send.call(
            au.AppClientMethodCallParams(
                method="transferFrom",
                args=[account.address, account.address, 1000],
                box_references=[
                    box_ref(token_client.app_id, balances_box),
                    box_ref(token_client.app_id, allowances_box),
                ],
            )
        )


# --- postInit guard test ---


