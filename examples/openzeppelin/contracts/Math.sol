// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0) (utils/math/Math.sol)
// Flattened and assembly-free version for AVM compilation via puya-sol

pragma solidity ^0.8.20;

/**
 * @dev Standard math utilities missing in the Solidity language.
 * Based on OpenZeppelin v5.0.0 Math library, with all inline assembly
 * replaced by pure Solidity equivalents for AVM compatibility.
 */
library Math {
    enum Rounding {
        Floor,
        Ceil
    }

    /**
     * @dev Returns the largest of two numbers.
     */
    function max(uint256 a, uint256 b) internal pure returns (uint256) {
        return a > b ? a : b;
    }

    /**
     * @dev Returns the smallest of two numbers.
     */
    function min(uint256 a, uint256 b) internal pure returns (uint256) {
        return a < b ? a : b;
    }

    /**
     * @dev Returns the average of two numbers. The result is rounded towards zero.
     * Uses the identity `(a & b) + (a ^ b) / 2` to avoid overflow.
     */
    function average(uint256 a, uint256 b) internal pure returns (uint256) {
        return (a & b) + (a ^ b) / 2;
    }

    /**
     * @dev Returns the ceiling of the division of two numbers.
     * Reverts on division by zero.
     */
    function ceilDiv(uint256 a, uint256 b) internal pure returns (uint256) {
        require(b > 0, "Math: division by zero");
        return a == 0 ? 0 : (a - 1) / b + 1;
    }

    /**
     * @dev Calculates floor(x * y / denominator) with full precision.
     * Since AVM biguint does not overflow on multiplication, we can use
     * the straightforward approach.
     *
     * Reverts if denominator is zero.
     */
    function mulDiv(uint256 x, uint256 y, uint256 denominator) internal pure returns (uint256) {
        require(denominator > 0, "Math: division by zero");
        return (x * y) / denominator;
    }

    /**
     * @dev Calculates x * y / denominator with the specified rounding mode.
     */
    function mulDiv(uint256 x, uint256 y, uint256 denominator, Rounding rounding) internal pure returns (uint256 result) {
        result = mulDiv(x, y, denominator);
        if (rounding == Rounding.Ceil && (x * y) % denominator > 0) {
            result += 1;
        }
    }

    /**
     * @dev Returns the square root of a number using Newton's method.
     * If the number is not a perfect square, the result is rounded towards zero.
     */
    function sqrt(uint256 a) internal pure returns (uint256) {
        if (a == 0) {
            return 0;
        }

        // Initial estimate: find the highest bit set and use it to seed Newton's method.
        // We compute an initial guess that is at least as large as floor(sqrt(a)).
        uint256 result = 1;
        uint256 xAux = a;

        if (xAux >= (1 << 128)) {
            xAux >>= 128;
            result <<= 64;
        }
        if (xAux >= (1 << 64)) {
            xAux >>= 64;
            result <<= 32;
        }
        if (xAux >= (1 << 32)) {
            xAux >>= 32;
            result <<= 16;
        }
        if (xAux >= (1 << 16)) {
            xAux >>= 16;
            result <<= 8;
        }
        if (xAux >= (1 << 8)) {
            xAux >>= 8;
            result <<= 4;
        }
        if (xAux >= (1 << 4)) {
            xAux >>= 4;
            result <<= 2;
        }
        if (xAux >= (1 << 2)) {
            result <<= 1;
        }

        // Newton's method iterations (7 iterations is sufficient for 256-bit precision)
        result = (result + a / result) >> 1;
        result = (result + a / result) >> 1;
        result = (result + a / result) >> 1;
        result = (result + a / result) >> 1;
        result = (result + a / result) >> 1;
        result = (result + a / result) >> 1;
        result = (result + a / result) >> 1;

        return min(result, a / result);
    }

    /**
     * @dev Returns the square root with the specified rounding mode.
     */
    function sqrt(uint256 a, Rounding rounding) internal pure returns (uint256) {
        uint256 result = sqrt(a);
        if (rounding == Rounding.Ceil && result * result < a) {
            result += 1;
        }
        return result;
    }

    /**
     * @dev Returns the log in base 2 of a positive value, rounded towards zero.
     * Returns 0 if given 0.
     */
    function log2(uint256 value) internal pure returns (uint256) {
        uint256 result = 0;

        if (value >> 128 > 0) {
            value >>= 128;
            result += 128;
        }
        if (value >> 64 > 0) {
            value >>= 64;
            result += 64;
        }
        if (value >> 32 > 0) {
            value >>= 32;
            result += 32;
        }
        if (value >> 16 > 0) {
            value >>= 16;
            result += 16;
        }
        if (value >> 8 > 0) {
            value >>= 8;
            result += 8;
        }
        if (value >> 4 > 0) {
            value >>= 4;
            result += 4;
        }
        if (value >> 2 > 0) {
            value >>= 2;
            result += 2;
        }
        if (value >> 1 > 0) {
            result += 1;
        }

        return result;
    }

    /**
     * @dev Returns the log in base 2 with the specified rounding mode.
     */
    function log2(uint256 value, Rounding rounding) internal pure returns (uint256) {
        uint256 result = log2(value);
        if (rounding == Rounding.Ceil && (1 << result) < value) {
            result += 1;
        }
        return result;
    }

    /**
     * @dev Returns the log in base 10 of a positive value, rounded towards zero.
     * Returns 0 if given 0.
     */
    function log10(uint256 value) internal pure returns (uint256) {
        uint256 result = 0;

        if (value >= 10 ** 64) {
            value /= 10 ** 64;
            result += 64;
        }
        if (value >= 10 ** 32) {
            value /= 10 ** 32;
            result += 32;
        }
        if (value >= 10 ** 16) {
            value /= 10 ** 16;
            result += 16;
        }
        if (value >= 10 ** 8) {
            value /= 10 ** 8;
            result += 8;
        }
        if (value >= 10 ** 4) {
            value /= 10 ** 4;
            result += 4;
        }
        if (value >= 10 ** 2) {
            value /= 10 ** 2;
            result += 2;
        }
        if (value >= 10 ** 1) {
            result += 1;
        }

        return result;
    }

    /**
     * @dev Returns the log in base 10 with the specified rounding mode.
     */
    function log10(uint256 value, Rounding rounding) internal pure returns (uint256) {
        uint256 result = log10(value);
        if (rounding == Rounding.Ceil && 10 ** result < value) {
            result += 1;
        }
        return result;
    }

    /**
     * @dev Returns the log in base 256 of a positive value, rounded towards zero.
     * Returns 0 if given 0.
     */
    function log256(uint256 value) internal pure returns (uint256) {
        uint256 result = 0;

        if (value >> 128 > 0) {
            value >>= 128;
            result += 16;
        }
        if (value >> 64 > 0) {
            value >>= 64;
            result += 8;
        }
        if (value >> 32 > 0) {
            value >>= 32;
            result += 4;
        }
        if (value >> 16 > 0) {
            value >>= 16;
            result += 2;
        }
        if (value >> 8 > 0) {
            result += 1;
        }

        return result;
    }

    /**
     * @dev Returns the log in base 256 with the specified rounding mode.
     */
    function log256(uint256 value, Rounding rounding) internal pure returns (uint256) {
        uint256 result = log256(value);
        if (rounding == Rounding.Ceil && (1 << (result << 3)) < value) {
            result += 1;
        }
        return result;
    }
}

/**
 * @dev Test contract for the Math library.
 */
contract MathTest {
    using Math for uint256;

    function testMulDiv(uint256 x, uint256 y, uint256 denominator) external pure returns (uint256) {
        return Math.mulDiv(x, y, denominator);
    }

    function testSqrt(uint256 a) external pure returns (uint256) {
        return Math.sqrt(a);
    }

    function testLog2(uint256 value) external pure returns (uint256) {
        return Math.log2(value);
    }

    function testLog10(uint256 value) external pure returns (uint256) {
        return Math.log10(value);
    }

    function testCeilDiv(uint256 a, uint256 b) external pure returns (uint256) {
        return Math.ceilDiv(a, b);
    }

    function testAverage(uint256 a, uint256 b) external pure returns (uint256) {
        return Math.average(a, b);
    }

    function testMax(uint256 a, uint256 b) external pure returns (uint256) {
        return Math.max(a, b);
    }

    function testMin(uint256 a, uint256 b) external pure returns (uint256) {
        return Math.min(a, b);
    }
}
