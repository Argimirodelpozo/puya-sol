"""Scratch-slot seam behaviour for 32-byte memory accesses.

A slot is 4096 bytes, a multiple of 32, so an access at a 32-aligned offset
can never cross a slot boundary and the straddle arm is dead code. An
unaligned access can cross and must stitch two slots. These pin both sides,
including writes that land exactly on the 4096 seam: a wrong alignment proof
does not fail loudly, it silently reads or writes the wrong bytes.
"""
import pytest

SOURCE = "puyasolRegression/contracts/mem_slot_seam.sol"
A = int("11" * 32, 16)
B = int("22" * 32, 16)
M = int("33" * 16 + "44" * 16, 16)
AFTER = int("44" * 16 + "22" * 16, 16)   # M's low half over B's high half


@pytest.fixture
def app(harness):
    return harness.compile_and_deploy(SOURCE, "MemSeam")


def _ok(harness, app, sig, *args):
    r = harness.call(app, sig, *args, extra_fee=20_000)
    assert not r.reverted, f"{sig}{args}: {r.fail_message}"
    return r.abi_return


def test_aligned_round_trip(harness, app):
    for off32 in (0, 1, 10, 127):        # 127*32 = 4064, the last in-slot word
        assert _ok(harness, app, "alignedRoundTrip(uint256,uint256)", off32, A) == A


def test_unaligned_round_trip(harness, app):
    for skew in (1, 7, 31):
        assert _ok(harness, app,
                   "unalignedRoundTrip(uint256,uint256,uint256)", 10, skew, A) == A


def test_seam_crossing_round_trip(harness, app):
    # 4064+skew for skew>0 starts before the seam and ends past it.
    for skew in (1, 16, 31):
        assert _ok(harness, app, "seamRoundTrip(uint256,uint256)", skew, B) == B


def test_aligned_neighbours_either_side_of_seam(harness, app):
    assert _ok(harness, app, "seamNeighbours(uint256,uint256)", A, B) == [A, B]


def test_seam_crossing_write_lands_on_exact_bytes(harness, app):
    before_, crossed, after_ = _ok(
        harness, app, "seamCrossingKeepsNeighbours(uint256)", M)
    assert before_ == A          # 4032..4063 untouched
    assert crossed == M          # 4080..4111 reads back whole
    assert after_ == AFTER       # 4096..4111 overwritten, 4112..4127 kept
