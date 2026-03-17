"""Shared test infrastructure for puya-sol feature regression tests.

Each feature folder has:
  contracts/*.sol   — Solidity source exercising the feature
  test_*.py         — Python tests that compile, deploy, and verify

Tests use localnet and the puya-sol compiler binary.
"""
import hashlib
import subprocess
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.kmd import KMDClient
from algosdk.transaction import (
    ApplicationCreateTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
    PaymentTxn,
)
from algosdk.v2client.algod import AlgodClient
from algokit_utils.models.account import SigningAccount
import pytest
import os

ROOT = Path(__file__).parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT.parent / "puya" / ".venv" / "bin" / "puya"


@pytest.fixture(scope="session")
def algod_client() -> AlgodClient:
    config = au.ClientManager.get_default_localnet_config("algod")
    return au.ClientManager.get_algod_client(config)


@pytest.fixture(scope="session")
def kmd_client() -> KMDClient:
    config = au.ClientManager.get_default_localnet_config("kmd")
    return au.ClientManager.get_kmd_client(config)


@pytest.fixture(scope="session")
def localnet_clients(algod_client, kmd_client):
    return au.AlgoSdkClients(algod=algod_client, kmd=kmd_client)


@pytest.fixture(scope="session")
def account(localnet_clients):
    return au.AlgorandClient(localnet_clients).account.localnet_dispenser()


@pytest.fixture(scope="session")
def localnet(localnet_clients, account):
    client = au.AlgorandClient(localnet_clients)
    client.set_suggested_params_cache_timeout(0)
    client.account.set_signer_from_account(account)
    return client


def compile_sol(sol_path: Path, out_dir: Path) -> dict:
    """Compile a Solidity file and return paths to artifacts.

    Returns dict with keys: approval_teal, clear_teal, arc56_json, approval_bin
    for each contract found in the output directory.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(COMPILER), "--source", str(sol_path),
         "--output-dir", str(out_dir),
         "--puya-path", str(PUYA)],
        capture_output=True, text=True, timeout=120,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Compilation failed for {sol_path.name}:\n{result.stdout}\n{result.stderr}"
        )

    # Discover compiled contracts
    contracts = {}
    for arc56 in out_dir.glob("*.arc56.json"):
        name = arc56.stem.replace(".arc56", "")
        contracts[name] = {
            "arc56": arc56,
            "approval_teal": out_dir / f"{name}.approval.teal",
            "clear_teal": out_dir / f"{name}.clear.teal",
            "approval_bin": out_dir / f"{name}.approval.bin",
        }
    return contracts


def deploy(localnet, account, arc56_path, approval_teal_path, clear_teal_path,
           fund_amount=1_000_000, constructor_args=None, foreign_apps=None,
           extra_fee=0):
    """Deploy a compiled contract and return an AppClient."""
    app_spec = au.Arc56Contract.from_json(arc56_path.read_text())
    algod = localnet.client.algod

    approval_bin = encoding.base64.b64decode(
        algod.compile(approval_teal_path.read_text())["result"]
    )
    clear_bin = encoding.base64.b64decode(
        algod.compile(clear_teal_path.read_text())["result"]
    )

    max_size = max(len(approval_bin), len(clear_bin))
    extra_pages = max(0, (max_size - 1) // 2048)

    sp = algod.suggested_params()
    if extra_fee:
        sp.fee = max(sp.min_fee, sp.fee) + extra_fee
        sp.flat_fee = True

    txn = ApplicationCreateTxn(
        sender=account.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_bin, clear_program=clear_bin,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=constructor_args or [],
        extra_pages=extra_pages,
        foreign_apps=foreign_apps or [],
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)

    app_id = result["application-index"]
    client = au.AppClient(
        au.AppClientParams(
            algorand=localnet, app_spec=app_spec,
            app_id=app_id, default_sender=account.address,
        )
    )
    if fund_amount > 0:
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
        )
        sp2 = algod.suggested_params()
        pay = PaymentTxn(account.address, sp2, app_addr, fund_amount)
        signed_pay = pay.sign(account.private_key)
        txid = algod.send_transaction(signed_pay)
        wait_for_confirmation(algod, txid, 4)
    return client


def mapping_box_key(mapping_name, *keys):
    concat_keys = b"".join(keys)
    key_hash = hashlib.sha256(concat_keys).digest()
    return mapping_name.encode() + key_hash


def box_ref(app_id, name):
    return au.BoxReference(app_id=app_id, name=name)


def addr_to_bytes32(addr):
    raw = encoding.decode_address(addr)
    return b"\x00" * (32 - len(raw)) + raw


def app_id_to_algod_addr(app_id):
    raw = b"\x00" * 24 + app_id.to_bytes(8, "big")
    return encoding.encode_address(raw)


NO_POPULATE = au.SendParams(populate_app_call_resources=False)
