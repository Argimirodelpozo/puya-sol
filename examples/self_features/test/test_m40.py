"""
M40: Cross-contract .call() return value decoding (Gap 1 verification).
Tests that .call(abi.encodeCall(...)) returns actual data from the callee,
not empty bytes, and that abi.decode() properly decodes the return data.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import PaymentTxn, wait_for_confirmation
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, deploy_contract_raw, load_arc56


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


def app_address(app_id: int) -> bytes:
    """Get the 32-byte address of an application."""
    return encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))


@pytest.fixture(scope="module")
def callee(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the ReturnCallee contract."""
    c = deploy_contract(localnet, account, "ReturnCallee")
    fund_contract(localnet, account, c.app_id)
    return c


def encode_app_id_as_address(app_id: int) -> bytes:
    """Encode app ID as 32-byte address with app ID in last 8 bytes.
    The compiler extracts app ID via extract(24, 8) from the stored address."""
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


@pytest.fixture(scope="module")
def caller(
    localnet: au.AlgorandClient, account: SigningAccount, callee: au.AppClient
) -> au.AppClient:
    """Deploy the ReturnCaller with the callee's app ID as constructor param."""
    callee_addr = encode_app_id_as_address(callee.app_id)
    app_spec = load_arc56("ReturnCaller")
    c = deploy_contract_raw(
        localnet, account, "ReturnCaller", app_spec,
        app_args=[callee_addr],
    )
    fund_contract(localnet, account, c.app_id)
    return c


# ── Return value tests ──

@pytest.mark.localnet
def test_call_get_number(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callGetNumber() should return 42 from the callee."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callGetNumber",
            args=[],
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return == 42


@pytest.mark.localnet
def test_call_add(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callAdd(10, 20) should return 30."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callAdd",
            args=[10, 20],
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return == 30


@pytest.mark.localnet
def test_call_add_large(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callAdd with larger numbers."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callAdd",
            args=[100, 200],
            note=b"large",
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return == 300


@pytest.mark.localnet
def test_call_is_even_true(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callIsEven(4) should return true."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callIsEven",
            args=[4],
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_call_is_even_false(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callIsEven(7) should return false."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callIsEven",
            args=[7],
            note=b"odd",
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_target_address(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """Verify the stored target address has the callee app ID."""
    result = caller.send.call(
        au.AppClientMethodCallParams(method="target", args=[])
    )
    expected_addr = encoding.encode_address(encode_app_id_as_address(callee.app_id))
    assert result.abi_return == expected_addr
