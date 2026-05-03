"""End-to-end tests for the AERC20 prototype.

The AERC20 base contract creates an ASA in `__postInit` and exposes a
standard ERC20 surface (balanceOf / transfer / approve / allowance /
transferFrom / totalSupply) backed by clawback `axfer` inner transactions.

These tests cover:

  * Deploy succeeds; the contract creates its underlying ASA.
  * `asaId()` exposes the new ASA id.
  * `totalSupply()` returns the initial supply (1_000_000).
  * `balanceOf(contract_address)` == total supply (creator holds the lot).
  * `balanceOf(non_opted_in_user)` == 0.
  * `approve` / `allowance` round-trip (no ASA interaction).
"""

from __future__ import annotations

import algokit_utils as au
import pytest
from algokit_utils.models.account import SigningAccount

from conftest import app_address, deploy_aerc20, fund_account, opt_in_to_asa

INITIAL_SUPPLY = 1_000_000
DECIMALS = 6


@pytest.fixture(scope="module")
def token_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_aerc20(localnet, account, "MyToken")


@pytest.mark.localnet
def test_deploys(token_client: au.AppClient) -> None:
    assert token_client.app_id > 0


@pytest.mark.localnet
def test_asa_was_created(token_client: au.AppClient) -> None:
    """`asaId()` should return a non-zero ASA id post-deploy."""
    result = token_client.send.call(au.AppClientMethodCallParams(method="asaId"))
    assert result.abi_return is not None
    assert int(result.abi_return) > 0


@pytest.mark.localnet
def test_total_supply(token_client: au.AppClient) -> None:
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    assert int(result.abi_return) == INITIAL_SUPPLY


@pytest.mark.localnet
def test_decimals(token_client: au.AppClient) -> None:
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="decimals")
    )
    assert int(result.abi_return) == DECIMALS


@pytest.mark.localnet
def test_symbol(token_client: au.AppClient) -> None:
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "MTK"


@pytest.mark.localnet
def test_name(token_client: au.AppClient) -> None:
    result = token_client.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "My Token"


@pytest.mark.localnet
def test_contract_holds_full_supply(token_client: au.AppClient) -> None:
    """The contract is the reserve / clawback / manager; on creation it
    owns the entire ASA balance."""
    contract_addr = app_address(token_client.app_id)
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[contract_addr],
        )
    )
    assert int(result.abi_return) == INITIAL_SUPPLY


@pytest.mark.localnet
def test_balance_zero_for_non_opted_in(
    token_client: au.AppClient, account: SigningAccount
) -> None:
    """The deployer hasn't opted into the ASA; balanceOf reads 0."""
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
        )
    )
    assert int(result.abi_return) == 0


@pytest.mark.localnet
def test_approve_then_allowance(
    token_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> None:
    """approve() writes to the allowance mapping and allowance() reads it."""
    spender = localnet.account.random()

    token_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[spender.address, 12345],
        )
    )
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, spender.address],
        )
    )
    assert int(result.abi_return) == 12345


@pytest.mark.localnet
def test_mint_to_opted_in_user(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """End-to-end clawback path: deploy fresh, opt user in, mint to them,
    confirm ASA balance moved from the contract reserve to the user."""
    client = deploy_aerc20(localnet, account, "MyToken")
    asa_id = int(
        client.send.call(au.AppClientMethodCallParams(method="asaId")).abi_return
    )

    user = localnet.account.random()
    fund_account(localnet, account, user, 1_000_000)
    opt_in_to_asa(localnet, user, asa_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user.address, 100],
            extra_fee=au.AlgoAmount.from_micro_algo(2_000),
        )
    )

    user_bal = int(
        client.send.call(
            au.AppClientMethodCallParams(method="balanceOf", args=[user.address])
        ).abi_return
    )
    assert user_bal == 100

    contract_bal = int(
        client.send.call(
            au.AppClientMethodCallParams(
                method="balanceOf", args=[app_address(client.app_id)]
            )
        ).abi_return
    )
    assert contract_bal == INITIAL_SUPPLY - 100


@pytest.mark.localnet
def test_user_to_user_transfer(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """Mint to A, A transfers to B, balances move accordingly."""
    client = deploy_aerc20(localnet, account, "MyToken")
    asa_id = int(
        client.send.call(au.AppClientMethodCallParams(method="asaId")).abi_return
    )

    user_a = localnet.account.random()
    user_b = localnet.account.random()
    fund_account(localnet, account, user_a, 1_000_000)
    fund_account(localnet, account, user_b, 1_000_000)
    opt_in_to_asa(localnet, user_a, asa_id)
    opt_in_to_asa(localnet, user_b, asa_id)

    # bootstrap A with 500 from the contract reserve
    client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[user_a.address, 500],
            extra_fee=au.AlgoAmount.from_micro_algo(2_000),
        )
    )

    # A → B (40)
    client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            sender=user_a.address,
            args=[user_b.address, 40],
            extra_fee=au.AlgoAmount.from_micro_algo(2_000),
        )
    )

    a_bal = int(
        client.send.call(
            au.AppClientMethodCallParams(
                method="balanceOf", args=[user_a.address]
            )
        ).abi_return
    )
    b_bal = int(
        client.send.call(
            au.AppClientMethodCallParams(
                method="balanceOf", args=[user_b.address]
            )
        ).abi_return
    )
    assert a_bal == 460
    assert b_bal == 40
