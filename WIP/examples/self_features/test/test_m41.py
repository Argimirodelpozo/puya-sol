"""
M41: Struct/tuple return from inner calls (Gap 6 verification).
Tests that high-level cross-contract calls returning tuples are decoded correctly.
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


def encode_app_id_as_address(app_id: int) -> bytes:
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


@pytest.fixture(scope="module")
def callee(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    c = deploy_contract(localnet, account, "StructCallee")
    fund_contract(localnet, account, c.app_id)
    return c


@pytest.fixture(scope="module")
def caller(
    localnet: au.AlgorandClient, account: SigningAccount, callee: au.AppClient
) -> au.AppClient:
    callee_addr = encode_app_id_as_address(callee.app_id)
    app_spec = load_arc56("StructCaller")
    c = deploy_contract_raw(
        localnet, account, "StructCaller", app_spec,
        app_args=[callee_addr],
    )
    fund_contract(localnet, account, c.app_id)
    return c


@pytest.mark.localnet
def test_call_get_point(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callGetPoint(5, 10) should return (10, 30) via tuple return decoding."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callGetPoint",
            args=[5, 10],
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return == {"rx": 10, "ry": 30}


@pytest.mark.localnet
def test_call_get_point_zeros(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callGetPoint(0, 0) should return (0, 0)."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callGetPoint",
            args=[0, 0],
            note=b"zeros",
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return == {"rx": 0, "ry": 0}


@pytest.mark.localnet
def test_call_get_sum(
    caller: au.AppClient, callee: au.AppClient
) -> None:
    """callGetSum(7, 3) should return 10."""
    result = caller.send.call(
        au.AppClientMethodCallParams(
            method="callGetSum",
            args=[7, 3],
            static_fee=au.AlgoAmount.from_micro_algo(2000),
        ),
        au.SendParams(
            populate_app_call_resources=True,
        ),
    )
    assert result.abi_return == 10
