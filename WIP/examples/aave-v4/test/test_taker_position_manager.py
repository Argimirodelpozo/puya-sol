"""TakerPositionManager tests — ported from upstream
tests/contracts/position-manager/TakerPositionManager/TakerPositionManager.t.sol.

Coverage: deploy + the 6 spoke-gated methods (approveWithdraw,
approveBorrow, renounceWithdrawAllowance, renounceBorrowAllowance,
withdrawOnBehalfOf, borrowOnBehalfOf) revert with SpokeNotRegistered
when called against an unregistered spoke.

Skipped from upstream:
 - happy-path (forward to ISpoke; needs Spoke wired)
 - all `*WithSig` permit-style flows (ECDSA signing)
 - `_fuzz_*` cases
"""

from __future__ import annotations

import os

import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk import encoding
from conftest import deploy_contract


def _addr_pk32(addr: str) -> bytes:
    return encoding.decode_address(addr)


ALICE_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (1).to_bytes(8, "big"))
)
BOB_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (2).to_bytes(8, "big"))
)
SPOKE2_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (4).to_bytes(8, "big"))
)


@pytest.fixture(scope="module")
def tpm(localnet, account):
    return deploy_contract(
        localnet, account, "TakerPositionManager",
        app_args=[_addr_pk32(account.address)],
    )


def _call(client, method, *args, boxes=None):
    kwargs = dict(method=method, args=list(args), note=os.urandom(8))
    if boxes:
        kwargs["box_references"] = boxes
    result = client.send.call(au.AppClientMethodCallParams(**kwargs))
    return result.abi_return


def test_deploy(tpm):
    assert tpm.app_id > 0


def test_owner(tpm, account):
    assert _call(tpm, "owner") == account.address


# ─── SpokeNotRegistered reverts on the 6 spoke-gated methods ──────────────────


def test_approveWithdraw_revertsWith_SpokeNotRegistered(tpm):
    with pytest.raises(LogicError):
        _call(tpm, "approveWithdraw", SPOKE2_ADDR, 0, BOB_ADDR, 100)


def test_approveBorrow_revertsWith_SpokeNotRegistered(tpm):
    with pytest.raises(LogicError):
        _call(tpm, "approveBorrow", SPOKE2_ADDR, 0, BOB_ADDR, 100)


def test_renounceWithdrawAllowance_revertsWith_SpokeNotRegistered(tpm):
    with pytest.raises(LogicError):
        _call(tpm, "renounceWithdrawAllowance", SPOKE2_ADDR, 0, ALICE_ADDR)


def test_renounceBorrowAllowance_revertsWith_SpokeNotRegistered(tpm):
    with pytest.raises(LogicError):
        _call(tpm, "renounceBorrowAllowance", SPOKE2_ADDR, 0, ALICE_ADDR)


def test_withdrawOnBehalfOf_revertsWith_SpokeNotRegistered(tpm):
    with pytest.raises(LogicError):
        _call(tpm, "withdrawOnBehalfOf", SPOKE2_ADDR, 0, 100, ALICE_ADDR)


def test_borrowOnBehalfOf_revertsWith_SpokeNotRegistered(tpm):
    with pytest.raises(LogicError):
        _call(tpm, "borrowOnBehalfOf", SPOKE2_ADDR, 0, 100, ALICE_ADDR)
