"""Runtime-offset memory ranges crossing the scratch-slot boundary, and the
runtime-out-offset returndata copy (previously silently skipped)."""

import hashlib

from framework import as_int


def test_identity_cross_slot(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_multislot_rt_copies.sol",
        contract_name="MultiSlotRtCopies",
        extra_args=["--evm-layout"],
    )
    # src in slot 1, dst in slot 2 — both far from slot 0.
    r = harness.call(app, "identityCross(uint256,uint256)", 5000, 9200).abi_return
    assert bytes(r[0]) == b"\x11" * 32, bytes(r[0]).hex()
    assert bytes(r[1]) == b"\x22" * 32, bytes(r[1]).hex()
    # Same-slot control (both in slot 0) still works.
    r = harness.call(app, "identityCross(uint256,uint256)", 256, 512).abi_return
    assert bytes(r[0]) == b"\x11" * 32


def test_sha256_cross_slot(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_multislot_rt_copies.sol",
        contract_name="MultiSlotRtCopies",
        extra_args=["--evm-layout"],
    )
    want = hashlib.sha256(b"\x41" * 32).digest()
    got = bytes(harness.call(app, "shaCross(uint256)", 6000).abi_return)
    assert got == want, got.hex()


def test_mcopy_dynamic_cross_slot(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_multislot_rt_copies.sol",
        contract_name="MultiSlotRtCopies",
        extra_args=["--evm-layout"],
    )
    got = bytes(
        harness.call(app, "mcopyCross(uint256,uint256,uint256)", 5100, 9300, 32).abi_return
    )
    assert got == b"\x33" * 32, got.hex()


def test_runtime_out_offset_returndata(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_multislot_rt_copies.sol",
        contract_name="RtOutCaller",
        extra_args=["--evm-layout"],
    )
    assert as_int(harness.call(app, "h(uint256,uint256)", 7, 0x40).abi_return) == 1007
    # Out buffer past the slot boundary too.
    assert as_int(harness.call(app, "h(uint256,uint256)", 8, 5200).abi_return) == 1008
