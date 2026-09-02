"""Inline-assembly (Yul) op audit vs the EVM — oracle answers pinned
2026-09-02 on solc 0.8.28 + py-evm (run log in the audit memory). All values
uint256 words; negatives are their two's-complement renderings.
"""
import pytest

SOURCE = "puyasolRegression/contracts/asm_parity_matrix.sol"
M = 1 << 256


@pytest.fixture
def app(harness):
    return harness.compile_and_deploy(SOURCE, "AsmParity")


def _ok(harness, app, sig, *args):
    r = harness.call(app, sig, *args, extra_fee=20_000)
    assert not r.reverted, f"{sig}{args}: {r.fail_message}"
    ret = r.abi_return
    vals = list(ret) if isinstance(ret, (list, tuple)) else [ret]
    return [v % M if isinstance(v, int) else v for v in vals]


def test_zero_divisors_return_zero(harness, app):
    assert _ok(harness, app, "divmodZero()") == [0, 0, 0, 0]


def test_sdiv_min_neg_one_wraps(harness, app):
    assert _ok(harness, app, "sdivMinNegOne()") == [1 << 255]


def test_smod_takes_dividend_sign(harness, app):
    assert _ok(harness, app, "smodSigns()") == [M - 1, 1]


def test_sdiv_truncates_toward_zero(harness, app):
    assert _ok(harness, app, "sdivRounding()") == [M - 3, M - 3]


def test_exp_wraps(harness, app):
    assert _ok(harness, app, "expCells(uint256,uint256)", 0, 0) == [1]
    assert _ok(harness, app, "expCells(uint256,uint256)", 3, 5) == [243]
    assert _ok(harness, app, "expCells(uint256,uint256)", 2, 256) == [0]
    assert _ok(harness, app, "expCells(uint256,uint256)", 1 << 255, 2) == [0]


def test_byte_out_of_range_is_zero(harness, app):
    assert _ok(harness, app, "byteCells()") == [0x11, 0x99, 0]


def test_signextend_cells(harness, app):
    assert _ok(harness, app, "signextendCells()") == [M - 128, 0x7F, 0x80]


def test_signed_compare_boundary(harness, app):
    assert _ok(harness, app, "signedCompareBoundary()") == [1, 1, 0]


def test_shift_saturation(harness, app):
    assert _ok(harness, app, "shiftSaturation()") == [0, 0, M - 1, 0]


def test_reads_past_end_are_zero(harness, app):
    assert _ok(harness, app, "calldataPastEnd()") == [0]
    assert _ok(harness, app, "mloadFreshZero()") == [0]


def test_addmod_mulmod_huge_and_zero_modulus(harness, app):
    assert _ok(harness, app, "addmodMulmodHuge()") == [2, 1, 0]


def test_not_iszero(harness, app):
    assert _ok(harness, app, "notIszero()") == [M - 256, 1, 0]
