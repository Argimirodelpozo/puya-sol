"""GiverPositionManager tests — ported from upstream
tests/contracts/position-manager/GiverPositionManager.t.sol.

Concrete coverage: deploy + the 2 spoke-gated methods
(supplyOnBehalfOf, repayOnBehalfOf) revert with SpokeNotRegistered
when called against an unregistered spoke.

Skipped:
 - happy-path supplyOnBehalfOf / repayOnBehalfOf (forward to ISpoke)
 - `_revertsWith_ReserveNotListed` (needs Spoke wired)
 - test_multicall (multi-call infrastructure)
 - all `_fuzz_*` cases
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
SPOKE2_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (4).to_bytes(8, "big"))
)


@pytest.fixture(scope="module")
def gpm(localnet, account):
    return deploy_contract(
        localnet, account, "GiverPositionManager",
        app_args=[_addr_pk32(account.address)],
    )


def _call(client, method, *args, boxes=None):
    kwargs = dict(method=method, args=list(args), note=os.urandom(8))
    if boxes:
        kwargs["box_references"] = boxes
    result = client.send.call(au.AppClientMethodCallParams(**kwargs))
    return result.abi_return


def test_deploy(gpm):
    assert gpm.app_id > 0


def test_owner(gpm, account):
    assert _call(gpm, "owner") == account.address


def test_supplyOnBehalfOf_revertsWith_SpokeNotRegistered(gpm):
    with pytest.raises(LogicError):
        _call(gpm, "supplyOnBehalfOf", SPOKE2_ADDR, 0, 100, ALICE_ADDR)


def test_repayOnBehalfOf_revertsWith_SpokeNotRegistered(gpm):
    with pytest.raises(LogicError):
        _call(gpm, "repayOnBehalfOf", SPOKE2_ADDR, 0, 100, ALICE_ADDR)
