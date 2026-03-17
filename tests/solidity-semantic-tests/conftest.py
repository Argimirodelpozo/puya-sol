"""Pytest integration for Solidity semantic tests.

Auto-discovers .sol test files, parses their assertions,
compiles with puya-sol, deploys to localnet, and verifies results.
"""
import subprocess
import json
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    PaymentTxn, wait_for_confirmation,
)
import pytest

from parser import parse_test_file, parse_value, SemanticTest, TestCall

ROOT = Path(__file__).parent.parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT.parent / "puya" / ".venv" / "bin" / "puya"
TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"
NO_POPULATE = au.SendParams(populate_app_call_resources=False)


@pytest.fixture(scope="session")
def localnet():
    algod = au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod"))
    kmd = au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd"))
    client = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
    client.set_suggested_params_cache_timeout(0)
    account = client.account.localnet_dispenser()
    client.account.set_signer_from_account(account)
    return client, account


def compile_sol(sol_path, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(COMPILER), "--source", str(sol_path),
         "--output-dir", str(out_dir),
         "--puya-path", str(PUYA)],
        capture_output=True, text=True, timeout=120,
    )
    if result.returncode != 0:
        return None
    contracts = {}
    for arc56 in out_dir.glob("*.arc56.json"):
        name = arc56.stem.replace(".arc56", "")
        contracts[name] = {
            "arc56": arc56,
            "approval_teal": out_dir / f"{name}.approval.teal",
            "clear_teal": out_dir / f"{name}.clear.teal",
        }
    return contracts


def deploy_app(localnet, account, artifacts):
    try:
        app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())
        algod = localnet.client.algod
        approval_bin = encoding.base64.b64decode(
            algod.compile(artifacts["approval_teal"].read_text())["result"])
        clear_bin = encoding.base64.b64decode(
            algod.compile(artifacts["clear_teal"].read_text())["result"])
        extra_pages = max(0, (max(len(approval_bin), len(clear_bin)) - 1) // 2048)
        sp = algod.suggested_params()
        txn = ApplicationCreateTxn(
            sender=account.address, sp=sp,
            on_complete=OnComplete.NoOpOC,
            approval_program=approval_bin, clear_program=clear_bin,
            global_schema=StateSchema(num_uints=16, num_byte_slices=16),
            local_schema=StateSchema(num_uints=0, num_byte_slices=0),
            extra_pages=extra_pages,
        )
        txid = algod.send_transaction(txn.sign(account.private_key))
        result = wait_for_confirmation(algod, txid, 4)
        app_id = result["application-index"]
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big")))
        sp2 = algod.suggested_params()
        pay = PaymentTxn(account.address, sp2, app_addr, 1_000_000)
        wait_for_confirmation(algod,
            algod.send_transaction(pay.sign(account.private_key)), 4)
        return au.AppClient(au.AppClientParams(
            algorand=localnet, app_spec=app_spec,
            app_id=app_id, default_sender=account.address))
    except Exception:
        return None


def compare_values(actual, expected):
    if expected is None:
        return True
    if isinstance(expected, bool):
        return actual is expected
    if isinstance(expected, int):
        if isinstance(actual, bool):
            return (1 if actual else 0) == expected
        if isinstance(actual, int):
            return actual == expected
        if isinstance(actual, (list, tuple)):
            try:
                actual_int = int.from_bytes(bytes(actual), 'big')
                return actual_int == expected
            except (ValueError, OverflowError):
                pass
    if isinstance(expected, bytes):
        if isinstance(actual, bytes):
            if actual == expected:
                return True
            if actual.rstrip(b'\x00') == expected.rstrip(b'\x00'):
                return True
            return False
        if isinstance(actual, (list, tuple)):
            actual_bytes = bytes(actual)
            if actual_bytes == expected:
                return True
            if actual_bytes.rstrip(b'\x00') == expected.rstrip(b'\x00'):
                return True
            return False
    return str(actual) == str(expected)
