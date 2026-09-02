"""Free-memory-pointer alignment invariant.

The pointer starts at 0x80 and Solidity's allocator only bumps it by aligned
amounts, so `add(pMem, k)` with aligned k is 32-aligned and its slot-straddle
arm is dead code. The invariant is inductive: it holds only if EVERY write
back to the pointer in the block stores a provably aligned value, so one
unaligned bump must fall the whole block back to the straddling path. These
pin the semantics on both sides; the elision itself is a size win, not a
behaviour change, so every value here must be identical either way.
"""
import pytest

SOURCE = "puyasolRegression/contracts/fmp_alignment.sol"
V = int("5a" * 32, 16)
W = int("a5" * 32, 16)


@pytest.fixture
def app(harness):
    return harness.compile_and_deploy(SOURCE, "FmpAlignment")


def _ok(harness, app, sig, *args):
    r = harness.call(app, sig, *args, extra_fee=20_000)
    assert not r.reverted, f"{sig}{args}: {r.fail_message}"
    return r.abi_return


def test_aligned_bump_round_trip(harness, app):
    assert _ok(harness, app, "alignedBump(uint256)", V) == V


def test_unaligned_bump_round_trip(harness, app):
    assert _ok(harness, app, "unalignedBump(uint256)", V) == V


def test_unaligned_bump_neighbours(harness, app):
    assert _ok(harness, app, "unalignedBumpNeighbours(uint256,uint256)", V, W) == [V, W]
