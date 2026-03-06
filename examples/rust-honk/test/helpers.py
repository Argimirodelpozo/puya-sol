"""
rust-honk Test Helpers — deploy, group calls, budget padding, proof encoding.
"""
import json
from pathlib import Path

import algokit_utils as au
from algosdk.transaction import (
    ApplicationCallTxn, ApplicationCreateTxn, OnComplete,
    StateSchema, wait_for_confirmation,
)
from algosdk.abi import Method
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer, TransactionWithSigner, AccountTransactionSigner,
)
from algosdk import encoding

FIXTURES_DIR = Path(__file__).parent / "fixtures"


# --- Loading test vectors ---

def load_proof_bytes(name: str) -> bytes:
    """Load proof as raw bytes from fixtures/<name>.json."""
    data = json.loads((FIXTURES_DIR / f"{name}.json").read_text())
    return bytes.fromhex(data["hex"])


def load_public_inputs() -> list[bytes]:
    """Load public inputs as list of 32-byte values."""
    data = json.loads((FIXTURES_DIR / "public_inputs.json").read_text())
    return [bytes.fromhex(h) for h in data]


# --- Deploy helpers ---

def compute_extra_pages(out_dir: Path, name: str) -> int:
    """Compute extra pages needed from approval binary size."""
    bin_path = out_dir / f"{name}.approval.bin"
    if not bin_path.exists():
        return 0
    size = bin_path.stat().st_size
    if size <= 2048:
        return 0
    return (size - 1) // 2048


def load_arc56(out_dir: Path, name: str) -> au.Arc56Contract:
    arc56_path = out_dir / f"{name}.arc56.json"
    return au.Arc56Contract.from_json(arc56_path.read_text())


def fund_contract(localnet, account, app_id, amount=1_000_000):
    algod = localnet.client.algod
    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )
    from algosdk.transaction import PaymentTxn
    sp = algod.suggested_params()
    txn = PaymentTxn(account.address, sp, app_addr, amount)
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def deploy_raw(localnet, account, out_dir, name, extra_pages=0):
    algod = localnet.client.algod
    app_spec = load_arc56(out_dir, name)
    approval_path = out_dir / f"{name}.approval.teal"
    clear_path = out_dir / f"{name}.clear.teal"
    approval_result = algod.compile(approval_path.read_text())
    clear_result = algod.compile(clear_path.read_text())
    approval_program = encoding.base64.b64decode(approval_result["result"])
    clear_program = encoding.base64.b64decode(clear_result["result"])

    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_program,
        clear_program=clear_program,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]

    return au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=app_spec,
            app_id=app_id,
            default_sender=account.address,
        )
    )


def deploy_contract(localnet, account, out_dir, name, orch_app_id=0,
                    fund_amount=1_000_000):
    """Deploy a contract (orchestrator or helper), fund it, and init auth."""
    extra_pages = compute_extra_pages(out_dir, name)
    client = deploy_raw(localnet, account, out_dir, name, extra_pages=extra_pages)

    if fund_amount > 0:
        fund_contract(localnet, account, client.app_id, fund_amount)

    if orch_app_id > 0:
        client.send.call(au.AppClientMethodCallParams(
            method="__init__",
            args=[orch_app_id, 0, b''],
        ))

    return client


# --- Method resolution ---

def resolve_method(helper, method_name):
    """Resolve an ABI method from a helper's app spec."""
    for m in helper.app_spec.methods:
        if m.name == method_name:
            return Method.from_signature(
                f"{m.name}({','.join(str(a.type) for a in m.args)}){m.returns.type}"
            )
    raise ValueError(f"Method {method_name} not found in app spec")


# --- Group call utilities ---

def simulate_grouped_call(helper, method_name, args, orchestrator, algod, account,
                          extra_opcode_budget=320_000):
    """Call ABI method via simulate with orchestrator auth + extra opcode budget."""
    abi_method = resolve_method(helper, method_name)
    sp = algod.suggested_params()
    signer = AccountTransactionSigner(account.private_key)
    atc = AtomicTransactionComposer()

    # Position 0: orchestrator __auth__
    auth_method = Method.from_signature("__auth__()void")
    atc.add_method_call(
        app_id=orchestrator.app_id,
        method=auth_method,
        sender=account.address,
        sp=sp,
        signer=signer,
    )

    # Actual method call
    atc.add_method_call(
        app_id=helper.app_id,
        method=abi_method,
        sender=account.address,
        sp=sp,
        signer=signer,
        method_args=args,
    )

    from algosdk.v2client.models import SimulateRequest
    sim_request = SimulateRequest(
        txn_groups=[],
        allow_more_logs=True,
        extra_opcode_budget=extra_opcode_budget,
        allow_empty_signatures=True,
        allow_unnamed_resources=True,
    )
    result = atc.simulate(algod, sim_request)
    return result


def simulate_verify(orchestrator, algod, account, proof_bytes, public_inputs,
                    extra_opcode_budget=320_000):
    """Call verify(proof, publicInputs) via group: [verify(...), __finish_verify()].

    Option A architecture: orchestrator at both ends of the group.
    Position 0: verify(proof, pis) — stores args to scratch, sets flag, returns true
    Position N: __finish_verify() — reads result from prev txn scratch, clears flag
    """
    verify_method = resolve_method(orchestrator, "verify")
    finish_method = resolve_method(orchestrator, "__finish_verify")
    sp = algod.suggested_params()
    signer = AccountTransactionSigner(account.private_key)
    atc = AtomicTransactionComposer()

    # Encode public inputs as list of 32-byte arrays
    pi_encoded = [list(pi) for pi in public_inputs]

    # Position 0: orchestrator.verify(proof, publicInputs)
    atc.add_method_call(
        app_id=orchestrator.app_id,
        method=verify_method,
        sender=account.address,
        sp=sp,
        signer=signer,
        method_args=[proof_bytes, pi_encoded],
    )

    # Position N (last): orchestrator.__finish_verify()
    atc.add_method_call(
        app_id=orchestrator.app_id,
        method=finish_method,
        sender=account.address,
        sp=sp,
        signer=signer,
    )

    from algosdk.v2client.models import SimulateRequest
    sim_request = SimulateRequest(
        txn_groups=[],
        allow_more_logs=True,
        extra_opcode_budget=extra_opcode_budget,
        allow_empty_signatures=True,
        allow_unnamed_resources=True,
    )
    result = atc.simulate(algod, sim_request)
    return result


# --- Proof manipulation ---

def corrupt_proof_zeros(proof_bytes: bytes, offset: int, length: int) -> bytes:
    """Zero out a range of bytes in a proof."""
    proof = bytearray(proof_bytes)
    proof[offset:offset + length] = bytes(length)
    return bytes(proof)


def corrupt_proof_point(proof_bytes: bytes, offset: int,
                        point_x_byte31: int = 1, point_y_byte31: int = 3) -> bytes:
    """Set an invalid G1 curve point (1, 3) at the given offset.
    Zeroes 128 bytes, then sets x and y coordinate last bytes."""
    proof = bytearray(proof_bytes)
    proof[offset:offset + 128] = bytes(128)
    proof[offset + 31] = point_x_byte31
    proof[offset + 64 + 31] = point_y_byte31
    return bytes(proof)
