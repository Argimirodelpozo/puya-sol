// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0) (utils/math/SignedMath.sol)
// Adapted for AVM: explicit sign-aware comparisons since AVM biguint is unsigned.

pragma solidity ^0.8.20;

library SignedMath {
    uint256 private constant SIGN_BIT = uint256(1) << 255;

    /// @dev Check if an int256 value is negative (bit 255 set in two's complement).
    function _isNegative(int256 x) private pure returns (bool) {
        return uint256(x) >= SIGN_BIT;
    }

    /// @dev Returns the larger of two signed numbers.
    function max(int256 a, int256 b) internal pure returns (int256) {
        bool aNeg = _isNegative(a);
        bool bNeg = _isNegative(b);
        if (aNeg && !bNeg) return b;  // a negative, b positive → b is larger
        if (!aNeg && bNeg) return a;  // a positive, b negative → a is larger
        // Same sign: unsigned comparison preserves ordering within each half
        return uint256(a) > uint256(b) ? a : b;
    }

    /// @dev Returns the smaller of two signed numbers.
    function min(int256 a, int256 b) internal pure returns (int256) {
        bool aNeg = _isNegative(a);
        bool bNeg = _isNegative(b);
        if (aNeg && !bNeg) return a;  // a negative → a is smaller
        if (!aNeg && bNeg) return b;  // b negative → b is smaller
        return uint256(a) < uint256(b) ? a : b;
    }

    /// @dev Returns the average of two signed numbers, rounded toward zero.
    /// Uses XOR-with-sign-bit trick to map signed → unsigned ordering,
    /// then applies unsigned average, then maps back.
    function average(int256 a, int256 b) internal pure returns (int256) {
        // XOR with SIGN_BIT maps int256 ordering to uint256 ordering:
        // most_negative → 0, most_positive → 2^256-1
        uint256 au = uint256(a) ^ SIGN_BIT;
        uint256 bu = uint256(b) ^ SIGN_BIT;
        // Unsigned average without overflow: (a&b) + (a^b)/2
        uint256 avg = (au & bu) + ((au ^ bu) >> 1);
        // Map back to int256: XOR with SIGN_BIT again
        int256 result = int256(avg ^ SIGN_BIT);
        // Floor division rounds toward -infinity. Correct to round toward zero:
        // If result is negative and the sum was odd, add 1.
        bool resultNeg = _isNegative(result);
        bool sumOdd = ((au ^ bu) & 1) == 1;
        if (resultNeg && sumOdd) {
            // Add 1 to round toward zero instead of toward -infinity
            result = int256(uint256(result) + 1);
        }
        return result;
    }

    /// @dev Returns the absolute unsigned value of a signed value.
    function abs(int256 n) internal pure returns (uint256) {
        if (_isNegative(n)) {
            // Two's complement: |n| = 2^256 - uint256(n) = max_uint256 - uint256(n) + 1
            return type(uint256).max - uint256(n) + 1;
        }
        return uint256(n);
    }
}

contract SignedMathTest {
    function testMax(int256 a, int256 b) external pure returns (int256) {
        return SignedMath.max(a, b);
    }

    function testMin(int256 a, int256 b) external pure returns (int256) {
        return SignedMath.min(a, b);
    }

    function testAverage(int256 a, int256 b) external pure returns (int256) {
        return SignedMath.average(a, b);
    }

    function testAbs(int256 n) external pure returns (uint256) {
        return SignedMath.abs(n);
    }
}
