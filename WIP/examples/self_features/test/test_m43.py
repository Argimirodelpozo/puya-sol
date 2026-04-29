"""
M43: Multiple contracts in a single .sol file (Gap 4 verification).
Tests that both TokenA and TokenB compile and deploy from one file.
"""

import pytest
import algokit_utils as au
from pathlib import Path
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema, PaymentTxn, wait_for_confirmation,
)
from algokit_utils.models.account import SigningAccount

OUT_DIR = Path(__file__).parent.parent / "out" / "MultiContract"


def load_arc56(contract_name: str) -> au.Arc56Contract:
    path = OUT_DIR / f"{contract_name}.arc56.json"
    return au.Arc56Contract.from_json(path.read_text())


def deploy(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    contract_name: str,
) -> au.AppClient:
    algod = localnet.client.algod
    app_spec = load_arc56(contract_name)
    approval = encoding.base64.b64decode(
        algod.compile((OUT_DIR / f"{contract_name}.approval.teal").read_text())["result"]
    )
    clear = encoding.base64.b64decode(
        algod.compile((OUT_DIR / f"{contract_name}.clear.teal").read_text())["result"]
    )
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]

    # Fund contract
    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )
    fund_txn = PaymentTxn(account.address, sp, app_addr, 1_000_000)
    algod.send_transaction(fund_txn.sign(account.private_key))

    return au.AppClient(
        au.AppClientParams(
            algorand=localnet, app_spec=app_spec,
            app_id=app_id, default_sender=account.address,
        )
    )


@pytest.fixture(scope="module")
def token_a(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy(localnet, account, "TokenA")


@pytest.fixture(scope="module")
def token_b(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy(localnet, account, "TokenB")


@pytest.mark.localnet
def test_token_a_mint(token_a: au.AppClient, account: SigningAccount) -> None:
    """TokenA.mint should increase balance."""
    token_a.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 100],
        ),
        au.SendParams(populate_app_call_resources=True),
    )
    result = token_a.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
        ),
        au.SendParams(populate_app_call_resources=True),
    )
    assert result.abi_return == 100


@pytest.mark.localnet
def test_token_b_mint(token_b: au.AppClient, account: SigningAccount) -> None:
    """TokenB.mint should increase balance and totalMinted."""
    token_b.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 200],
        ),
        au.SendParams(populate_app_call_resources=True),
    )
    result = token_b.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
        ),
        au.SendParams(populate_app_call_resources=True),
    )
    assert result.abi_return == 200

    result2 = token_b.send.call(
        au.AppClientMethodCallParams(method="totalMinted", args=[]),
    )
    assert result2.abi_return == 200


@pytest.mark.localnet
def test_both_independent(
    token_a: au.AppClient, token_b: au.AppClient, account: SigningAccount,
) -> None:
    """TokenA and TokenB are independent — different app IDs, separate state."""
    assert token_a.app_id != token_b.app_id
