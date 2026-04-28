"""Probe: does `app_params_get AppApprovalProgram` work for an approval
program > 4096 bytes (the AVM stack-value max)?

We deploy a tiny probe app whose approval reads another app's approval
bytes via `app_params_get` and logs the resulting length. The target app
is v2's helper1 (5147 bytes) — well past the 4KB stack-value max.

If the probe call returns a logged length matching helper1's full size,
then the AVM allows stack values larger than 4096 for program-read
opcodes specifically (and our (C) plan is fine). If it errors at
runtime, we fall back to the box-storage approach.
"""
import base64
import sys
from pathlib import Path

import algokit_utils as au
from algosdk.transaction import (
    ApplicationCreateTxn,
    ApplicationCallTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
    PaymentTxn,
)


PROBE_TEAL = """\
#pragma version 10
#pragma typetrack false

main:
    intcblock 0 1
    txn ApplicationID
    bnz call_path
    intc_1
    return

call_path:
    txn OnCompletion
    !
    assert
    intc_0
    txna ApplicationArgs 0
    btoi
    app_params_get AppApprovalProgram
    assert
    len
    itob
    log
    intc_1
    return
"""

CLEAR_TEAL = """\
#pragma version 10
pushint 1
return
"""


def main():
    target_dir = Path(__file__).parent.parent / "out" / "exchange" / "CTFExchange" / "CTFExchange__Helper1"
    target_bin = target_dir / "CTFExchange__Helper1.approval.bin"
    if not target_bin.exists():
        print(f"target not built: {target_bin}", file=sys.stderr)
        sys.exit(1)
    target_size = target_bin.stat().st_size
    print(f"target (helper1) approval size: {target_size} bytes")

    algod = au.ClientManager.get_algod_client(au.ClientManager.get_default_localnet_config("algod"))
    kmd = au.ClientManager.get_kmd_client(au.ClientManager.get_default_localnet_config("kmd"))
    clients = au.AlgoSdkClients(algod=algod, kmd=kmd)
    admin = au.AlgorandClient(clients).account.localnet_dispenser()
    localnet = au.AlgorandClient(clients)
    localnet.set_suggested_params_cache_timeout(0)
    localnet.account.set_signer_from_account(admin)

    # 1. Deploy the target — helper1's actual TEAL with template vars stubbed.
    target_teal = (target_dir / "CTFExchange__Helper1.approval.teal").read_text()
    target_clear = (target_dir / "CTFExchange__Helper1.clear.teal").read_text()
    target_approval = base64.b64decode(algod.compile(target_teal)["result"])
    target_clear_bin = base64.b64decode(algod.compile(target_clear)["result"])

    sp = algod.suggested_params()
    extra_pages = max(0, (max(len(target_approval), len(target_clear_bin)) - 1) // 2048)
    target_create = ApplicationCreateTxn(
        sender=admin.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=target_approval,
        clear_program=target_clear_bin,
        global_schema=StateSchema(num_uints=0, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
    )
    txid = algod.send_transaction(target_create.sign(admin.private_key))
    res = wait_for_confirmation(algod, txid, 4)
    target_app_id = res["application-index"]
    print(f"deployed target app: {target_app_id}")

    # 2. Deploy the probe.
    probe_approval = base64.b64decode(algod.compile(PROBE_TEAL)["result"])
    probe_clear = base64.b64decode(algod.compile(CLEAR_TEAL)["result"])
    probe_create = ApplicationCreateTxn(
        sender=admin.address, sp=algod.suggested_params(),
        on_complete=OnComplete.NoOpOC,
        approval_program=probe_approval,
        clear_program=probe_clear,
        global_schema=StateSchema(num_uints=0, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
    )
    txid = algod.send_transaction(probe_create.sign(admin.private_key))
    res = wait_for_confirmation(algod, txid, 4)
    probe_app_id = res["application-index"]
    print(f"deployed probe app: {probe_app_id}")

    # 3. Call probe with target_app_id as ApplicationArgs[0].
    probe_call = ApplicationCallTxn(
        sender=admin.address, sp=algod.suggested_params(),
        index=probe_app_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[target_app_id.to_bytes(8, "big")],
        foreign_apps=[target_app_id],
    )
    try:
        txid = algod.send_transaction(probe_call.sign(admin.private_key))
        res = wait_for_confirmation(algod, txid, 4)
        logs = res.get("logs", [])
        if not logs:
            print("ERROR: probe didn't log anything")
            sys.exit(2)
        logged_len = int.from_bytes(base64.b64decode(logs[0]), "big")
        print(f"probe logged length: {logged_len}")
        if logged_len == target_size:
            print(f"OK: app_params_get returned full {target_size} bytes "
                  f"(>= 4096) without truncation")
        else:
            print(f"PARTIAL: logged {logged_len} != actual {target_size}")
    except Exception as e:
        print(f"FAIL: probe errored: {e}")
        sys.exit(3)


if __name__ == "__main__":
    main()
