"""Zero-divisor guard elision for constant divisors.

`safeDivMod` wraps every div/mod in `d != 0 ? a op d : 0` because the AVM
panics where the EVM returns 0. When the divisor is a compile-time non-zero
constant the guard is unreachable, and each one costs three basic blocks:
poseidon's 816 mulmods over one field prime chained ~2500 of them into a
single body, which puya's SSA reader walks recursively until it runs out of
stack. Values pinned against EVM semantics (Python big-int arithmetic).
"""
import pytest

SOURCE = "puyasolRegression/contracts/div_guard_constant_fold.sol"
F = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture
def app(harness):
    return harness.compile_and_deploy(SOURCE, "DivGuardFold")


def _ok(harness, app, sig, *args):
    r = harness.call(app, sig, *args, extra_fee=20_000)
    assert not r.reverted, f"{sig}{args}: {r.fail_message}"
    return r.abi_return


def test_mulmod_constant_modulus_is_full_precision(harness, app):
    # (F-1)*2 exceeds F; 2^255*2^255 exceeds 2^256, so a wrapped multiply diverges.
    assert _ok(harness, app, "mulmodConst(uint256,uint256)", F - 1, 2) == F - 2
    assert _ok(harness, app, "mulmodConst(uint256,uint256)", 2**255, 2**255) == \
        5708294888247120917224517499881755159259782813036369317078999261412233599850


def test_addmod_constant_modulus_wraps(harness, app):
    assert _ok(harness, app, "addmodConst(uint256,uint256)", F - 1, 5) == 4


def test_mod_small_constant(harness, app):
    assert _ok(harness, app, "modSmallConst(uint256)", 100) == 3


def test_zero_divisor_still_returns_zero(harness, app):
    assert _ok(harness, app, "divByLiteralZero(uint256)", 5) == 0
    assert _ok(harness, app, "divByZeroLocal(uint256)", 5) == 0
    assert _ok(harness, app, "divByRuntime(uint256,uint256)", 7, 0) == 0
    assert _ok(harness, app, "divByRuntime(uint256,uint256)", 7, 2) == 3


def test_reassigned_local_keeps_its_guard(harness, app):
    assert _ok(harness, app, "modByReassigned(uint256,bool)", 100, False) == 3
    assert _ok(harness, app, "modByReassigned(uint256,bool)", 100, True) == 0
