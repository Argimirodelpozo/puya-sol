"""Function pointers to PUBLIC multi-return targets.

Previously the dispatch skipped such entries (wire-tuple decode
unimplemented) and any call through the pointer hit the invalid-pointer
assert. Now the dispatch types the target call with computeReturnPlan's
wire tuple and adapts each element back to native.

Also guards the Panic(0x51) revert payload for an uninitialized internal
function pointer (solc's panic code for that shape).
"""

SOURCE = "puyasolRegression/contracts/funcptr_multireturn.sol"
PANIC_51 = bytes.fromhex("4e487b71") + (0x51).to_bytes(32, "big")


def test_public_multireturn_through_pointer(harness):
    app = harness.compile_and_deploy(SOURCE)
    r = harness.call(app, "callMulti(uint256)", 1, extra_fee=10_000)
    assert not r.reverted, r.fail_message
    vals = list(r.abi_return)
    assert vals[0] == 10 ** 30
    assert vals[1] == 42
    assert vals[2] == -7 or vals[2] == (1 << 256) - 7  # decoder-dependent


def test_public_multireturn_aggregate_through_pointer(harness):
    app = harness.compile_and_deploy(SOURCE)
    r = harness.call(app, "callMultiArr()", extra_fee=10_000)
    assert not r.reverted, r.fail_message
    assert list(r.abi_return) == [5, 6, 11]


def test_uninitialized_pointer_panics_0x51(harness):
    app = harness.compile_and_deploy(SOURCE)
    r = harness.call(app, "callMulti(uint256)", 0,
                     extra_fee=10_000, expect_revert=True)
    assert r.reverted
    assert r.revert_data == PANIC_51, r.revert_data.hex()
