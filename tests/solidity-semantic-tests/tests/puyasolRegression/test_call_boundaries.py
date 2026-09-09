"""Call lowering must consume solc identities, write facts, and sequencing."""

import pytest
from algosdk.encoding import encode_address
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int, as_signed_int


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
def test_pointer_bindings_and_forms(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/call_boundary_pointers.sol",
        contract_name="CallBoundaryPointers", via_yul_behavior=via_ir,
        extra_args=["--evm-storage-layout"] if slot_layout else [])
    for second, expected in ((False, 32), (True, 5)):
        assert as_int(harness.call(app, "reassigned(bool)", second).abi_return) == expected
        result = harness.call(app, "forms(bool)", second).abi_return
        assert tuple(map(as_int, result)) == (expected,) * 4
    for signature, expected in (
        ("swapped()", 532), ("loop()", 320505),
        ("assemblyReassigned()", 5), ("externalReassigned()", 5),
        ("stableReference()", 10),
    ):
        assert as_int(harness.call(app, signature).abi_return) == expected
    assert as_int(harness.call(app, "cleared(bool)", False).abi_return) == 32
    assert harness.call(app, "cleared(bool)", True, expect_revert=True).reverted


@pytest.mark.parametrize("via_ir", [False, True])
def test_pointer_operand_order(harness, via_ir):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/call_boundary_pointers.sol",
        contract_name="CallBoundaryPointers", via_yul_behavior=via_ir)
    for second, expected in ((False, 11), (True, 2)):
        result = harness.call(app, "sequenced(bool)", second).abi_return
        assert tuple(map(as_int, result)) == (expected, 2)
    result = harness.call(app, "calleeOrder()").abi_return
    assert tuple(map(as_int, result)) == (23, 123 if via_ir else 231)
    result = harness.call(app, "assignedInArgument()").abi_return
    assert as_int(result) == (23 if via_ir else 5)
    result = harness.call(app, "stateCalleeOrder()").abi_return
    assert as_int(result) == (23 if via_ir else 5)


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
def test_parameter_and_return_boundaries(harness, via_ir, slot_layout):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/call_boundary_values.sol",
        contract_name="CallBoundaryValues", via_yul_behavior=via_ir,
        extra_args=["--evm-storage-layout"] if slot_layout else [])
    result = harness.call(app, "contexts(bytes,uint16)", b"\xaf\x01\x02", 300).abi_return
    assert tuple(map(as_int, result)) == (475, 475, 475)
    result = harness.call(app, "directScalar()").abi_return
    # ARC-4 publishes signed returns as sign-extended uint256 carriers.
    assert tuple(map(as_signed_int, result)) == (-123, -123)
    for other, expected in ((False, -123), (True, -22)):
        result = harness.call(app, "pointerScalar(bool)", other).abi_return
        assert as_signed_int(result) == expected
    for signature in ("directTuple()", "pointerTuple()"):
        result = harness.call(app, signature).abi_return
        assert (as_signed_int(result[0]), as_int(result[1]), as_int(result[2])) == (
            -123, 251, (1 << 100) + 7)
    for signature, args in (("selfRecord()", []), ("selfKeyedRecord(uint128)", [(1 << 80) + 9])):
        result = harness.call(app, signature, *args).abi_return
        assert (as_int(result[0]), as_signed_int(result[1]), as_int(result[2]), bytes(result[3])) == (
            251, -123, (1 << 100) + 7, b"\x01\x02\x03")


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_external_pointer_return_boundary(harness, via_ir, profile):
    artifacts = harness.compile(
        "puyasolRegression/contracts/call_boundary_values.sol",
        via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    caller = harness.deploy(artifacts, "CallBoundaryValues")
    receiver = harness.deploy(artifacts, "CallBoundaryValues")
    target = (encode_address(receiver.app_id.to_bytes(32, "big")) if profile == "arc4"
              else "0x" + receiver.app_id.to_bytes(20, "big").hex())

    def call(name, returns):
        signature = name + "(address)"
        if profile == "arc4":
            result = harness.call(caller, signature, target, extra_fee=30_000)
            assert not result.reverted, result.fail_message
            return result.abi_return
        selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
        result = harness.call_raw(caller, selector, extra_args=(encode(["address"], [target]),),
                                  extra_fee=30_000)
        assert not result.reverted, result.fail_message
        values = decode(returns, result.logs[-1][4:])
        return values[0] if len(returns) == 1 else values

    for name in ("remoteTuple", "directRemoteTuple"):
        result = call(name, ["int16", "uint8", "uint128"])
        assert (as_signed_int(result[0]), as_int(result[1]), as_int(result[2])) == (
            -123, 251, (1 << 100) + 7)
    for name in ("remoteBytes", "directRemoteBytes"):
        assert bytes(call(name, ["bytes"])) == b"\x00\xff\x01\x02"
    result = call("remotePair", ["int16", "int16"])
    assert tuple(map(as_signed_int, result)) == (-123, -22)
