"""Tests for PRBMath UD60x18 compiled to AVM via puya-sol."""
import pytest
from conftest import UNIT, HALF_UNIT

# ── msb (most significant bit) ──────────────────────────────────────────────

class TestMSB:
    def test_msb_one(self, call):
        assert call("msb", 1) == 0

    def test_msb_two(self, call):
        assert call("msb", 2) == 1

    def test_msb_four(self, call):
        assert call("msb", 4) == 2

    def test_msb_power_of_two(self, call):
        assert call("msb", 2**128) == 128

    def test_msb_large(self, call):
        assert call("msb", 2**255) == 255

    def test_msb_not_power(self, call):
        # msb(7) = 2  (binary 111 → bit 2)
        assert call("msb", 7) == 2

    def test_msb_256(self, call):
        assert call("msb", 256) == 8


# ── mulDiv (512-bit precision) ───────────────────────────────────────────────

class TestMulDiv:
    def test_simple(self, call):
        assert call("mulDiv", 6, 7, 3) == 14

    def test_large_numerator(self, call):
        # (2^128 * 2^128) / 2^128 = 2^128
        val = 2**128
        assert call("mulDiv", val, val, val) == val

    def test_unit_precision(self, call):
        # 1e18 * 1e18 / 1e18 = 1e18
        assert call("mulDiv", UNIT, UNIT, UNIT) == UNIT

    def test_rounding_toward_zero(self, call):
        # 10 * 3 / 7 = 4 (rounds down from 4.28...)
        assert call("mulDiv", 10, 3, 7) == 4


# ── mulDiv18 (x*y / 1e18) ───────────────────────────────────────────────────

class TestMulDiv18:
    def test_one_times_one(self, call):
        # 1e18 * 1e18 / 1e18 = 1e18
        assert call("mulDiv18", UNIT, UNIT) == UNIT

    def test_two_times_three(self, call):
        assert call("mulDiv18", 2 * UNIT, 3 * UNIT) == 6 * UNIT

    def test_half_times_two(self, call):
        assert call("mulDiv18", HALF_UNIT, 2 * UNIT) == UNIT


# ── commonSqrt (integer square root) ────────────────────────────────────────

class TestCommonSqrt:
    def test_zero(self, call):
        assert call("commonSqrt", 0) == 0

    def test_one(self, call):
        assert call("commonSqrt", 1) == 1

    def test_four(self, call):
        assert call("commonSqrt", 4) == 2

    def test_perfect_square(self, call):
        assert call("commonSqrt", 10000) == 100

    def test_non_perfect_rounds_down(self, call):
        # sqrt(8) = 2 (rounds down from 2.82...)
        assert call("commonSqrt", 8) == 2

    def test_large(self, call):
        assert call("commonSqrt", 10**36) == 10**18


# ── commonExp2 (2^x in 192.64 fixed-point) ──────────────────────────────────

class TestCommonExp2:
    def test_zero(self, call):
        # exp2(0) in 192.64 format: 0 → result = 1e18
        assert call("commonExp2", 0) == UNIT

    def test_one(self, call):
        # exp2(1 in 192.64) = exp2(2^64) = 2e18
        assert call("commonExp2", 2**64) == 2 * UNIT


# ── avg ──────────────────────────────────────────────────────────────────────

class TestAvg:
    def test_same_values(self, call):
        assert call("avg", 4 * UNIT, 4 * UNIT) == 4 * UNIT

    def test_different_values(self, call):
        assert call("avg", 2 * UNIT, 4 * UNIT) == 3 * UNIT

    def test_zero_and_value(self, call):
        assert call("avg", 0, 6 * UNIT) == 3 * UNIT

    def test_odd_sum(self, call):
        # avg(1, 2) rounds toward zero: (1+2)/2 = 1
        assert call("avg", 1, 2) == 1


# ── ceil ─────────────────────────────────────────────────────────────────────

class TestCeil:
    def test_whole_number(self, call):
        assert call("ceil", 3 * UNIT) == 3 * UNIT

    def test_fractional(self, call):
        assert call("ceil", 3 * UNIT + 1) == 4 * UNIT

    def test_zero(self, call):
        assert call("ceil", 0) == 0

    def test_half(self, call):
        assert call("ceil", HALF_UNIT) == UNIT


# ── floor ────────────────────────────────────────────────────────────────────

class TestFloor:
    def test_whole_number(self, call):
        assert call("floor", 3 * UNIT) == 3 * UNIT

    def test_fractional(self, call):
        assert call("floor", 3 * UNIT + HALF_UNIT) == 3 * UNIT

    def test_zero(self, call):
        assert call("floor", 0) == 0


# ── frac ─────────────────────────────────────────────────────────────────────

class TestFrac:
    def test_whole_number(self, call):
        assert call("frac", 5 * UNIT) == 0

    def test_half(self, call):
        assert call("frac", 5 * UNIT + HALF_UNIT) == HALF_UNIT

    def test_zero(self, call):
        assert call("frac", 0) == 0


# ── div (x / y in UD60x18) ──────────────────────────────────────────────────

class TestDiv:
    def test_one_div_one(self, call):
        assert call("div", UNIT, UNIT) == UNIT

    def test_two_div_one(self, call):
        assert call("div", 2 * UNIT, UNIT) == 2 * UNIT

    def test_six_div_three(self, call):
        assert call("div", 6 * UNIT, 3 * UNIT) == 2 * UNIT

    def test_one_div_two(self, call):
        assert call("div", UNIT, 2 * UNIT) == HALF_UNIT


# ── mul (x * y in UD60x18) ──────────────────────────────────────────────────

class TestMul:
    def test_one_times_one(self, call):
        assert call("mul", UNIT, UNIT) == UNIT

    def test_two_times_three(self, call):
        assert call("mul", 2 * UNIT, 3 * UNIT) == 6 * UNIT

    def test_by_zero(self, call):
        assert call("mul", 5 * UNIT, 0) == 0


# ── inv (1/x in UD60x18) ────────────────────────────────────────────────────

class TestInv:
    def test_one(self, call):
        assert call("inv", UNIT) == UNIT

    def test_two(self, call):
        assert call("inv", 2 * UNIT) == HALF_UNIT


# ── sqrt (UD60x18 square root) ──────────────────────────────────────────────

class TestSqrt:
    def test_one(self, call):
        assert call("sqrt", UNIT) == UNIT

    def test_four(self, call):
        assert call("sqrt", 4 * UNIT) == 2 * UNIT

    def test_zero(self, call):
        assert call("sqrt", 0) == 0


# ── gm (geometric mean) ─────────────────────────────────────────────────────

class TestGm:
    def test_same_values(self, call):
        assert call("gm", 4 * UNIT, 4 * UNIT) == 4 * UNIT

    def test_zero(self, call):
        assert call("gm", 0, 4 * UNIT) == 0


# ── exp (e^x) ───────────────────────────────────────────────────────────────

class TestExp:
    def test_zero(self, call):
        assert call("exp", 0) == UNIT  # e^0 = 1

    @pytest.mark.xfail(reason="exp2 Taylor series truncation in Yul codegen")
    def test_one(self, call):
        result = call("exp", UNIT)
        e = 2_718281828459045235  # e * 1e18
        # Allow 0.01% tolerance
        assert abs(result - e) < e // 10000


# ── exp2 (2^x) ──────────────────────────────────────────────────────────────

class TestExp2:
    def test_zero(self, call):
        assert call("exp2", 0) == UNIT  # 2^0 = 1

    def test_one(self, call):
        assert call("exp2", UNIT) == 2 * UNIT  # 2^1 = 2

    def test_two(self, call):
        assert call("exp2", 2 * UNIT) == 4 * UNIT  # 2^2 = 4


# ── log2 ─────────────────────────────────────────────────────────────────────

class TestLog2:
    def test_one(self, call):
        assert call("log2", UNIT) == 0  # log2(1) = 0

    def test_two(self, call):
        assert call("log2", 2 * UNIT) == UNIT  # log2(2) = 1

    def test_four(self, call):
        assert call("log2", 4 * UNIT) == 2 * UNIT  # log2(4) = 2

    def test_eight(self, call):
        assert call("log2", 8 * UNIT) == 3 * UNIT  # log2(8) = 3


# ── ln ───────────────────────────────────────────────────────────────────────

class TestLn:
    def test_one(self, call):
        assert call("ln", UNIT) == 0  # ln(1) = 0

    def test_e(self, call):
        e = 2_718281828459045235
        result = call("ln", e)
        # ln(e) ≈ 1e18, allow 0.01% tolerance
        assert abs(result - UNIT) < UNIT // 10000


# ── log10 ────────────────────────────────────────────────────────────────────

class TestLog10:
    def test_ten(self, call):
        assert call("log10", 10 * UNIT) == UNIT  # log10(10) = 1

    def test_hundred(self, call):
        assert call("log10", 100 * UNIT) == 2 * UNIT  # log10(100) = 2


# ── pow ──────────────────────────────────────────────────────────────────────

class TestPow:
    def test_zero_exp(self, call):
        assert call("pow", 5 * UNIT, 0) == UNIT  # x^0 = 1

    def test_one_exp(self, call):
        assert call("pow", 5 * UNIT, UNIT) == 5 * UNIT  # x^1 = x

    def test_square(self, call):
        result = call("pow", 2 * UNIT, 2 * UNIT)
        # 2^2 = 4
        assert abs(result - 4 * UNIT) < UNIT // 1000


# ── powu (integer exponent) ─────────────────────────────────────────────────

class TestPowu:
    def test_zero_exp(self, call):
        assert call("powu", 5 * UNIT, 0) == UNIT

    def test_one_exp(self, call):
        assert call("powu", 5 * UNIT, 1) == 5 * UNIT

    def test_square(self, call):
        assert call("powu", 3 * UNIT, 2) == 9 * UNIT

    def test_cube(self, call):
        assert call("powu", 2 * UNIT, 3) == 8 * UNIT
