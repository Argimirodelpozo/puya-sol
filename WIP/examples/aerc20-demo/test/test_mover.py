"""De-risk tests for using AERC20 (ASA-backed ERC20) as a V4 currency (#53).

V4's CurrencyLibrary.transfer/balanceOf make EXTERNAL calls to the currency
contract. The V4 no-hooks flow never exercises external calls, so these confirm
the mechanism works for AERC20:
  * external view call (balanceOf) resolves the target app + returns the ASA balance
  * a contract can opt into the ASA (AVM.asaOptIn), hold AERC20, and move it out via
    an external transfer call (asaTransfer clawback) — the V4 "take" shape.

puya-sol external-call convention (discovered here): the target address encodes the
app id in its LAST 8 bytes (extract_uint64(addr, 24)). So a V4 Currency for an
AERC20 must be 0x{24 zero bytes}{app_id}.
"""

import base64

import algokit_utils as au
import algosdk
from algosdk.transaction import (
    ApplicationCreateTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
)
from conftest import (  # type: ignore[import-not-found]
    OUT_DIR,
    app_address,
    deploy_aerc20,
    fund_account,
    load_arc56,
    opt_in_to_asa,
)


def _deploy_mover(localnet, account):
    algod = localnet.client.algod
    ap = base64.b64decode(algod.compile((OUT_DIR / "Mover.approval.teal").read_text())["result"])
    cl = base64.b64decode(algod.compile((OUT_DIR / "Mover.clear.teal").read_text())["result"])
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        account.address, sp, OnComplete.NoOpOC, ap, cl, StateSchema(0, 0), StateSchema(0, 0)
    )
    mid = wait_for_confirmation(
        algod, algod.send_transaction(txn.sign(account.private_key)), 4
    )["application-index"]
    client = au.AppClient(
        au.AppClientParams(
            algorand=localnet, app_spec=load_arc56("Mover"), app_id=mid, default_sender=account.address
        )
    )
    return client, mid


def _encoded(app_id: int) -> str:
    # puya-sol external-call target: app id in the address's last 8 bytes
    return algosdk.encoding.encode_address(bytes(24) + app_id.to_bytes(8, "big"))


def test_mover_external_view_call(localnet, account):
    """A contract's external balanceOf() call to an AERC20 returns the ASA balance."""
    token = deploy_aerc20(localnet, account, "MyToken")
    total = int(token.send.call(au.AppClientMethodCallParams(method="totalSupply")).abi_return)
    mover, _ = _deploy_mover(localnet, account)
    got = int(
        mover.send.call(
            au.AppClientMethodCallParams(
                method="checkBalance",
                args=[_encoded(token.app_id), app_address(token.app_id)],
                app_references=[token.app_id],
                extra_fee=au.AlgoAmount.from_micro_algo(2000),
            ),
            send_params={"populate_app_call_resources": True},
        ).abi_return
    )
    assert got == total


def test_mover_take(localnet, account):
    """A contract opts in, holds AERC20, and moves it out via an external transfer
    call (the V4 'take' shape: AERC20.transfer → asaTransfer clawback)."""
    token = deploy_aerc20(localnet, account, "MyToken")
    asa = int(token.send.call(au.AppClientMethodCallParams(method="asaId")).abi_return)
    mover, mover_id = _deploy_mover(localnet, account)
    mover_acct = app_address(mover_id)
    fund_account(localnet, account, mover_acct, 300_000)  # MBR for the ASA opt-in
    sp = {"populate_app_call_resources": True}
    mover.send.call(
        au.AppClientMethodCallParams(method="optIn", args=[asa], extra_fee=au.AlgoAmount.from_micro_algo(2000)),
        send_params=sp,
    )
    token.send.call(
        au.AppClientMethodCallParams(
            method="mint", args=[mover_acct, 1000], extra_fee=au.AlgoAmount.from_micro_algo(2000)
        ),
        send_params=sp,
    )
    opt_in_to_asa(localnet, account, asa)
    mover.send.call(
        au.AppClientMethodCallParams(
            method="move",
            args=[_encoded(token.app_id), account.address, 1000],
            app_references=[token.app_id],
            extra_fee=au.AlgoAmount.from_micro_algo(4000),
        ),
        send_params=sp,
    )
    bu = int(token.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[account.address])).abi_return)
    bm = int(token.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[mover_acct])).abi_return)
    assert bu == 1000 and bm == 0
