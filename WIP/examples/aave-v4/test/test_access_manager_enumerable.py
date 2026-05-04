"""End-to-end tests for AccessManagerEnumerable via --uros-splitter dance.

AME's 28 view methods are split across 3 chunks (compile_all.sh
specifies the groups). Each test:
  1. Deploys orch (session fixture) once.
  2. Deploys AME via deploy_split_app — substitutes TMPL_UROS_ORCH_APP_ID,
     deploys main, runs __postInit, registers chunks with orch.
  3. Calls non-split mutators (e.g. grantRole) directly on main.
  4. Calls split views via SplitDeployment.call_dance — sends a 2-txn
     group [main.viewMethod, orch.dispatch] and decodes the return log.

Confirms multi-chunk dance (one orch, N chunks) works on a real AAVE
V4 contract.
"""

from __future__ import annotations

import base64

import algokit_utils as au
from algokit_utils.models.account import SigningAccount
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCallTxn,
    OnComplete,
    wait_for_confirmation,
)
import pytest

from uros_dance import deploy_split_app, _arc4_selector


def _addr_to_pk32(addr: str) -> bytes:
    """Algorand address → 32-byte public-key. AME treats addresses as
    uint8[32] in ABI sigs."""
    return encoding.decode_address(addr)


def _decode_abi_log(log_b64: str) -> bytes:
    raw = base64.b64decode(log_b64)
    assert raw[:4] == bytes.fromhex("151f7c75"), \
        f"bad ABI return prefix: {raw[:4].hex()}"
    return raw[4:]


def test_deploy(localnet: au.AlgorandClient, account: SigningAccount, orch_app_id: int):
    """Smoke: AME deploys via dance + __postInit succeeds."""
    deployment = deploy_split_app(
        localnet.client.algod, account, "AccessManagerEnumerable",
        orch_id=orch_app_id,
        app_args=[_addr_to_pk32(account.address)],
    )
    assert deployment.main_id > 0
    assert deployment.orch_id == orch_app_id
    # 28 split methods × 3 chunks = 28 selectors registered.
    assert len(deployment.selector_to_chunk) == 28


def test_get_role_count_via_dance(
    localnet: au.AlgorandClient, account: SigningAccount, orch_app_id: int
):
    """Call a stubbed view (getRoleCount → uint256) via the dance and
    decode its return log. On a fresh deploy with only the admin role
    granted (which AME excludes from tracking), count is 0."""
    d = deploy_split_app(
        localnet.client.algod, account, "AccessManagerEnumerable",
        orch_id=orch_app_id,
        app_args=[_addr_to_pk32(account.address)],
    )

    # call_dance sends [main.getRoleCount(), orch.dispatch()]. The orch
    # runs the 3-itxn dance; the inner call's ABI return surfaces as a
    # log on the dispatch txn (the inner subcall).
    result = d.call_dance(
        localnet.client.algod, account, "getRoleCount()uint256",
    )
    # Find the ABI return log in the inner txns (orch's dispatch
    # forwards the inner call). Algod returns logs nested under
    # inner-txns of the dispatch.
    inner = result.get("inner-txns") or []
    found = None
    for itxn in inner:
        for itxn2 in itxn.get("inner-txns") or [itxn]:
            for log in itxn2.get("logs", []) or []:
                raw = base64.b64decode(log)
                if raw[:4] == bytes.fromhex("151f7c75"):
                    found = raw[4:]
                    break
            if found:
                break
        if found:
            break
    # Top-level logs are also possible (orch's dispatch returns the
    # inner call's return value).
    if not found:
        for log in result.get("logs", []) or []:
            raw = base64.b64decode(log)
            if raw[:4] == bytes.fromhex("151f7c75"):
                found = raw[4:]
                break
    assert found is not None, f"no ABI return log found; result keys: {list(result.keys())}"
    count = int.from_bytes(found, "big")
    assert count == 0, f"expected getRoleCount()=0 on fresh deploy, got {count}"
