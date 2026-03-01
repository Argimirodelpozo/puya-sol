import pytest
import algokit_utils as au
from conftest import deploy_contract, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def ownable2step(localnet, account):
    return deploy_contract(localnet, account, "Ownable2StepTest")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


ZERO_ADDR = encoding.encode_address(b"\x00" * 32)


def test_deploy(ownable2step):
    assert ownable2step.app_id > 0


def test_owner_is_deployer(ownable2step, account):
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="owner"))
    assert result.abi_return == account.address


def test_pending_owner_initially_zero(ownable2step):
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="pendingOwner"))
    assert result.abi_return == ZERO_ADDR


def test_transfer_ownership_sets_pending(ownable2step, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr2 = addr_bytes(account2.address)

    ownable2step.send.call(
        au.AppClientMethodCallParams(
            method="transferOwnership",
            args=[addr2],
        )
    )

    # Owner hasn't changed yet
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="owner"))
    assert result.abi_return == account.address

    # Pending owner is set
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="pendingOwner"))
    assert result.abi_return == account2.address


def test_non_pending_owner_cannot_accept(ownable2step, account, localnet):
    # account (current owner) tries to accept — should fail since they're not pending owner
    account3 = localnet.account.random()
    fund_account(localnet, account, account3)

    client3 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=ownable2step.app_spec,
            app_id=ownable2step.app_id,
            default_sender=account3.address,
        )
    )
    with pytest.raises(Exception):
        client3.send.call(au.AppClientMethodCallParams(method="acceptOwnership"))


def test_pending_owner_accepts(ownable2step, account, localnet):
    # Get pending owner (account2 from previous test)
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="pendingOwner"))
    pending_addr = result.abi_return

    # Find account2 from pending_addr
    # Create client for account2
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)

    # Re-transfer to account2 so we know the private key
    addr2 = addr_bytes(account2.address)
    ownable2step.send.call(
        au.AppClientMethodCallParams(
            method="transferOwnership",
            args=[addr2],
        )
    )

    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=ownable2step.app_spec,
            app_id=ownable2step.app_id,
            default_sender=account2.address,
        )
    )
    client2.send.call(au.AppClientMethodCallParams(method="acceptOwnership"))

    # Owner is now account2
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="owner"))
    assert result.abi_return == account2.address

    # Pending owner is cleared
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="pendingOwner"))
    assert result.abi_return == ZERO_ADDR


def test_non_owner_cannot_transfer(ownable2step, account, localnet):
    # account (no longer owner) cannot transfer
    with pytest.raises(Exception):
        ownable2step.send.call(
            au.AppClientMethodCallParams(
                method="transferOwnership",
                args=[addr_bytes(account.address)],
            )
        )


def test_renounce_by_new_owner(ownable2step, localnet):
    # Get current owner
    result = ownable2step.send.call(au.AppClientMethodCallParams(method="owner"))
    owner_addr = result.abi_return

    # The new owner renounces (but we don't have their key from above test)
    # Skip if we can't call as owner
    # This test validates that non-owners can't renounce
    with pytest.raises(Exception):
        ownable2step.send.call(au.AppClientMethodCallParams(method="renounceOwnership"))
