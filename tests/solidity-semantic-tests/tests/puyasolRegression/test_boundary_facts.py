"""Declaration/operand facts and the compact external-pointer boundary."""

import hashlib

import pytest
from Crypto.Hash import keccak
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_boundary_facts(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/boundary_facts.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile, "--evm-selectors"]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "IntrinsicBoundaryFacts", fund_wei=20_000_000)
    check = lambda sig, args=(), ret=("uint64",): invoke(harness, app, profile, sig, args, ret)
    digest = hashlib.sha3_256(b"boundary").digest()
    assert check("hashes(bytes)", [b"boundary"], ["bytes32"] * 3) == (bytes(31) + b"\x07", digest, digest)
    assert check("ordinaryGroup()") == (19,)
    assert check("named()", ret=["uint64"] * 3) == ((2, 0, 11) if via_ir else (2, 12, 0))
    assert check("capture()") == ((2,) if via_ir else (9,))
    assert check("bound()", ret=["uint64"] * 3) == ((2, 2, 0) if via_ir else (2, 0, 1))

    app = harness.deploy(artifacts, "PointerBoundaryFacts", fund_wei=20_000_000)
    peer = harness.deploy(artifacts, "PointerBoundaryFacts", fund_wei=20_000_000)
    address = peer.app_id.to_bytes(32, "big")
    selector = keccak.new(digest_bits=256, data=b"value(uint64)").digest()[:4]
    # ARC4's native pointer retains its routing selector; EVM mode retains both.
    wire = check("referenceWire(address)", [address], ["bytes"])[0]
    assert wire[:20] == address[-20:]
    assert wire[24:] == bytes(8)
    if profile == "evm":
        assert wire[20:24] == selector
    assert check("roundTrip(bytes)", [wire], ["bytes"]) == (wire,)
    assert check("roundTrip(bytes)", [bytes(32)], ["bytes"]) == (bytes(32),)
    assert check("cross(address,uint64)", [address, 5]) == (12,)
    assert check("self(uint64)", [6]) == (13,)
    invoke(harness, app, profile, "zero(uint64)", [0], reverts=True)
    # A rejected address must never be silently compacted into a valid app id.
    invalid = (2**128 + peer.app_id).to_bytes(32, "big")
    invoke(harness, app, profile, "referenceWire(address)", [invalid], reverts=True)
    invoke(harness, app, profile, "roundTrip(bytes)", [invalid[-20:] + selector + bytes(8)], reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_compact_pointer_facts(harness, via_ir, slot):
    artifacts = harness.compile("puyasolRegression/contracts/compact_pointer_facts.sol",
                                via_yul_behavior=via_ir,
                                extra_args=["--evm-storage-layout"] if slot else [])
    app = harness.deploy(artifacts, "CompactPointerFacts", fund_wei=20_000_000)
    peer = harness.deploy(artifacts, "CompactPointerFacts", fund_wei=20_000_000)
    address = peer.app_id.to_bytes(32, "big")
    assert invoke(harness, app, "arc4", "addressOf(address)", [address]) == (peer.app_id,)
    assert invoke(harness, app, "arc4", "addressOf(address)", [bytes(32)]) == (0,)
    assert invoke(harness, app, "arc4", "cross(address,uint64)", [address, 5], ["uint64"]) == (12,)
    assert invoke(harness, app, "arc4", "self(uint64)", [6], ["uint64"]) == (13,)
    invoke(harness, app, "arc4", "addressOf(address)", [(2**128 + peer.app_id).to_bytes(32, "big")], reverts=True)
