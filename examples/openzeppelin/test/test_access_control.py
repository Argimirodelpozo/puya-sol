"""OpenZeppelin AccessControl behavioral tests.

The compiler auto-splits the constructor when box writes are detected:
  1. Create app (no box refs)
  2. Fund app
  3. Call __postInit() with box refs (runs constructor body)
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key


DEFAULT_ADMIN_ROLE = b"\x00" * 32
MINTER_ROLE = b"\x01" + b"\x00" * 31  # arbitrary role identifier


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def has_role_box_key(role: bytes, account_addr: str) -> bytes:
    """Compute box key for _hasRole[role][account]."""
    raw_addr = encoding.decode_address(account_addr)
    return mapping_box_key("_hasRole", role, raw_addr)


def has_role_boxes(app_id: int, role: bytes, account_addr: str) -> list[au.BoxReference]:
    """Box refs for _hasRole[role][account]."""
    return [box_ref(app_id, has_role_box_key(role, account_addr))]


def admin_role_boxes(app_id: int, role: bytes) -> list[au.BoxReference]:
    """Box refs for _adminRoles[role]."""
    return [box_ref(app_id, mapping_box_key("_adminRoles", role))]


def deploy_access_control(localnet, account):
    """Deploy AccessControl with auto-generated __postInit.

    The compiler detects box writes in the constructor and auto-generates:
      1. Create (global state init only, sets __ctor_pending=1)
      2. Fund
      3. __postInit() runs constructor body (_grantRole)
    """
    init_box_key = has_role_box_key(DEFAULT_ADMIN_ROLE, account.address)
    client = deploy_contract(
        localnet, account, "AccessControlTest",
        post_init_boxes=[
            lambda app_id: box_ref(app_id, init_box_key),
        ],
    )
    return client


@pytest.mark.localnet
def test_deploys(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)
    assert client.app_id > 0


@pytest.mark.localnet
def test_deployer_has_admin_role(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[DEFAULT_ADMIN_ROLE, account.address],
            box_references=has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address),
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_non_admin_has_no_role(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)
    other_addr = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[DEFAULT_ADMIN_ROLE, other_addr],
            box_references=has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, other_addr),
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_get_role_admin_default(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getRoleAdmin",
            args=[DEFAULT_ADMIN_ROLE],
            box_references=admin_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE),
        )
    )
    assert result.abi_return == list(DEFAULT_ADMIN_ROLE) or result.abi_return == DEFAULT_ADMIN_ROLE


@pytest.mark.localnet
def test_grant_role(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)
    other_addr = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"

    all_boxes = (
        has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address)
        + admin_role_boxes(client.app_id, MINTER_ROLE)
        + has_role_boxes(client.app_id, MINTER_ROLE, other_addr)
    )
    client.send.call(
        au.AppClientMethodCallParams(
            method="grantRole",
            args=[MINTER_ROLE, other_addr],
            box_references=all_boxes,
        )
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[MINTER_ROLE, other_addr],
            box_references=has_role_boxes(client.app_id, MINTER_ROLE, other_addr),
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_grant_role_unauthorized_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)

    account2 = localnet.account.random()
    from algosdk.transaction import PaymentTxn, wait_for_confirmation
    algod = localnet.client.algod
    sp = algod.suggested_params()
    fund_txn = PaymentTxn(account.address, sp, account2.address, 10_000_000)
    signed = fund_txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)
    localnet.account.set_signer_from_account(account2)

    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=client.app_spec,
            app_id=client.app_id,
            default_sender=account2.address,
        )
    )
    all_boxes = (
        has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account2.address)
        + admin_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE)
        + has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account2.address)
    )
    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="grantRole",
                args=[DEFAULT_ADMIN_ROLE, account2.address],
                box_references=all_boxes,
            )
        )


@pytest.mark.localnet
def test_revoke_role(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)
    other_addr = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"

    grant_boxes = (
        has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address)
        + admin_role_boxes(client.app_id, MINTER_ROLE)
        + has_role_boxes(client.app_id, MINTER_ROLE, other_addr)
    )
    client.send.call(
        au.AppClientMethodCallParams(
            method="grantRole",
            args=[MINTER_ROLE, other_addr],
            box_references=grant_boxes,
        )
    )

    revoke_boxes = (
        has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address)
        + admin_role_boxes(client.app_id, MINTER_ROLE)
        + has_role_boxes(client.app_id, MINTER_ROLE, other_addr)
    )
    client.send.call(
        au.AppClientMethodCallParams(
            method="revokeRole",
            args=[MINTER_ROLE, other_addr],
            box_references=revoke_boxes,
        )
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[MINTER_ROLE, other_addr],
            box_references=has_role_boxes(client.app_id, MINTER_ROLE, other_addr),
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_renounce_role(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)

    renounce_boxes = has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address)
    client.send.call(
        au.AppClientMethodCallParams(
            method="renounceRole",
            args=[DEFAULT_ADMIN_ROLE, account.address],
            box_references=renounce_boxes,
        )
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[DEFAULT_ADMIN_ROLE, account.address],
            box_references=has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address),
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_renounce_bad_confirmation_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_access_control(localnet, account)

    other_addr = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"
    renounce_boxes = has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="renounceRole",
                args=[DEFAULT_ADMIN_ROLE, other_addr],
                box_references=renounce_boxes,
            )
        )


@pytest.mark.localnet
def test_postinit_twice_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """__postInit can only be called once (during deploy)."""
    client = deploy_access_control(localnet, account)
    init_boxes = has_role_boxes(client.app_id, DEFAULT_ADMIN_ROLE, account.address)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="__postInit",
                box_references=init_boxes,
            )
        )
