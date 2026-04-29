// SPDX-License-Identifier: MIT
pragma solidity >=0.8.8;

// ─── Custom Errors ─────────────────────────────────────────────────
library Errors {
    error InsufficientBalance();
    error InvalidAmount(uint256 amount);
}

// ─── User-Defined Value Type with Library ──────────────────────────
type Amount is uint256;

library AmountLib {
    function add(Amount a, Amount b) internal pure returns (Amount) {
        return Amount.wrap(Amount.unwrap(a) + Amount.unwrap(b));
    }

    function isZero(Amount a) internal pure returns (bool) {
        return Amount.unwrap(a) == 0;
    }

    function toUint(Amount a) internal pure returns (uint256) {
        return Amount.unwrap(a);
    }
}

using AmountLib for Amount;

// ─── Constants with Complex Initializers ───────────────────────────
uint256 constant MODULUS = 21888242871839275222246405745257275088548364400416034343698204186575808495617;
uint256 constant MODULUS_MINUS_ONE = MODULUS - 1;
uint256 constant TWO_POW_128 = 2 ** 128;
uint256 constant TWO_POW_64 = 2 ** 64;
uint256 constant SHIFT_14 = 1 << 14;
uint256 constant SHIFT_68 = 1 << 68;

// ─── Main Test Contract ────────────────────────────────────────────
contract EasyWinsTest {

    // ── Constant expression tests ──
    function testConstModulusMinusOne() external pure returns (uint256) {
        return MODULUS_MINUS_ONE;
    }

    function testConstTwoPow128() external pure returns (uint256) {
        return TWO_POW_128;
    }

    function testConstTwoPow64() external pure returns (uint256) {
        return TWO_POW_64;
    }

    function testConstShift14() external pure returns (uint256) {
        return SHIFT_14;
    }

    function testConstShift68() external pure returns (uint256) {
        return SHIFT_68;
    }

    // ── Custom error tests ──
    function testRequirePass(uint256 x) external pure returns (uint256) {
        require(x > 0, Errors.InsufficientBalance());
        return x;
    }

    function testRequireWithError(uint256 x) external pure returns (uint256) {
        require(x < 1000, Errors.InvalidAmount(x));
        return x;
    }

    // ── Using-for method calls on UDVT ──
    function testAmountAdd(uint256 a, uint256 b) external pure returns (uint256) {
        Amount amtA = Amount.wrap(a);
        Amount amtB = Amount.wrap(b);
        Amount result = amtA.add(amtB);
        return result.toUint();
    }

    function testAmountIsZero(uint256 x) external pure returns (bool) {
        Amount amt = Amount.wrap(x);
        return amt.isZero();
    }

    // ── bytes32 ↔ uint256 type cast ──
    function testBytes32FromUint(uint256 x) external pure returns (bytes32) {
        return bytes32(x);
    }

    function testUintFromBytes32(bytes32 x) external pure returns (uint256) {
        return uint256(x);
    }

    // ── bytes slicing + length (calldata only) ──
    function testBytesSliceAndLength(bytes calldata data) external pure returns (uint256) {
        // Slicing is only supported on calldata arrays
        bytes calldata firstHalf = data[0:data.length / 2];
        return firstHalf.length;
    }

    // ── Inline exponentiation (compile-time) ──
    function testInlinePow() external pure returns (uint256) {
        // 2**10 = 1024, should be folded at compile time
        return 2 ** 10;
    }

    // ── Revert statement with custom error ──
    function testRevertCustom(uint256 x) external pure returns (uint256) {
        if (x == 0)
            revert Errors.InsufficientBalance();
        return x;
    }

    // ── Combined: using-for + custom error ──
    function testCombined(uint256 a, uint256 b) external pure returns (uint256) {
        Amount amtA = Amount.wrap(a);
        require(!amtA.isZero(), Errors.InsufficientBalance());
        Amount amtB = Amount.wrap(b);
        Amount result = amtA.add(amtB);
        return result.toUint();
    }
}
