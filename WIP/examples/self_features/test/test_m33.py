"""
M33: Cross-contract call via .call(abi.encodeCall(...)).
Tests the SafeTransferLib pattern where low-level .call() is used
with abi.encodeCall to invoke interface methods on another contract.
"""

import hashlib

import pytest
import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import PaymentTxn, wait_for_confirmation
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

INNER_FEE = au.AlgoAmount(micro_algo=2000)


def mapping_box_key(mapping_name: str, *keys: bytes) -> bytes:
    concat_keys = b"".join(keys)
    key_hash = hashlib.sha256(concat_keys).digest()
    return mapping_name.encode() + key_hash


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def fund_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    app_id: int,
    amount: int = 1_000_000,
) -> None:
    algod = localnet.client.algod
    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )
    sp = algod.suggested_params()
    txn = PaymentTxn(account.address, sp, app_addr, amount)
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def app_address(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )


@pytest.fixture(scope="module")
def token_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy MockToken and mint initial supply to the caller contract."""
    client = deploy_contract(localnet, account, "MockToken")
    fund_contract(localnet, account, client.app_id, 2_000_000)
    return client


@pytest.fixture(scope="module")
def caller_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy SafeTransferCaller."""
    client = deploy_contract(localnet, account, "SafeTransferCaller")
    fund_contract(localnet, account, client.app_id, 1_000_000)
    return client


@pytest.mark.localnet
def test_safe_transfer(
    token_client: au.AppClient,
    caller_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> None:
    """
    SafeTransferCaller.safeTransfer(token, to, 100) should execute an inner
    app call to MockToken.transfer(to, 100) via abi.encodeCall.
    """
    token_app_id = token_client.app_id
    caller_app_id = caller_client.app_id

    # The caller contract's address (msg.sender for the inner call)
    caller_addr = app_address(caller_app_id)
    caller_addr_bytes = encoding.decode_address(caller_addr)

    # Recipient: use the test account
    recipient_addr = account.address
    recipient_addr_bytes = encoding.decode_address(recipient_addr)

    # Box keys for the token's _balances mapping
    sender_box = mapping_box_key("_balances", caller_addr_bytes)
    recipient_box = mapping_box_key("_balances", recipient_addr_bytes)

    # Mint tokens to the caller contract so it can transfer
    token_client.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[caller_addr, 500],
            box_references=[box_ref(token_app_id, sender_box)],
        )
    )

    # Verify mint worked
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[caller_addr],
            box_references=[box_ref(token_app_id, sender_box)],
        )
    )
    assert result.abi_return == 500

    # Now call safeTransfer on the caller contract
    # This should do: token.call(abi.encodeCall(IERC20.transfer, (to, 100)))
    # which becomes an inner app call to MockToken.transfer(to, 100)
    token_addr_as_bytes = token_app_id.to_bytes(32, "big")

    caller_client.send.call(
        au.AppClientMethodCallParams(
            method="safeTransfer",
            args=[
                token_addr_as_bytes,   # address token (app ID as 32-byte address)
                recipient_addr,        # address to
                100,                   # uint256 amount
            ],
            app_references=[token_app_id],
            box_references=[
                box_ref(token_app_id, sender_box),
                box_ref(token_app_id, recipient_box),
            ],
            extra_fee=INNER_FEE,
        )
    )

    # Verify the transfer happened: caller's balance should be 400
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[caller_addr],
            box_references=[box_ref(token_app_id, sender_box)],
        )
    )
    assert result.abi_return == 400

    # Recipient should have 100
    result = token_client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[recipient_addr],
            box_references=[box_ref(token_app_id, recipient_box)],
        )
    )
    assert result.abi_return == 100
