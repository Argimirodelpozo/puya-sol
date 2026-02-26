// SPDX-License-Identifier: MIT
pragma solidity >=0.8.8;

// ─── User-Defined Value Type ────────────────────────────────────
type Fr is uint256;

// ─── Free functions for operator overloading ────────────────────
function add(Fr a, Fr b) pure returns (Fr) {
    return Fr.wrap(Fr.unwrap(a) + Fr.unwrap(b));
}

function sub(Fr a, Fr b) pure returns (Fr) {
    return Fr.wrap(Fr.unwrap(a) - Fr.unwrap(b));
}

function mul(Fr a, Fr b) pure returns (Fr) {
    return Fr.wrap(Fr.unwrap(a) * Fr.unwrap(b));
}

function eq(Fr a, Fr b) pure returns (bool) {
    return Fr.unwrap(a) == Fr.unwrap(b);
}

function neq(Fr a, Fr b) pure returns (bool) {
    return Fr.unwrap(a) != Fr.unwrap(b);
}

using {add as +, sub as -, mul as *, eq as ==, neq as !=} for Fr global;

// ─── Enum ───────────────────────────────────────────────────────
enum Color { Red, Green, Blue, Yellow }

// ─── Contract ───────────────────────────────────────────────────
contract FeatureTest {
    // ── UDVT: wrap and unwrap ──
    function testWrap(uint256 x) external pure returns (uint256) {
        Fr val = Fr.wrap(x);
        return Fr.unwrap(val);
    }

    // ── UDVT: operator overloading (arithmetic) ──
    function testFrAdd(uint256 a, uint256 b) external pure returns (uint256) {
        Fr x = Fr.wrap(a);
        Fr y = Fr.wrap(b);
        Fr result = x + y;
        return Fr.unwrap(result);
    }

    function testFrSub(uint256 a, uint256 b) external pure returns (uint256) {
        Fr x = Fr.wrap(a);
        Fr y = Fr.wrap(b);
        Fr result = x - y;
        return Fr.unwrap(result);
    }

    function testFrMul(uint256 a, uint256 b) external pure returns (uint256) {
        Fr x = Fr.wrap(a);
        Fr y = Fr.wrap(b);
        Fr result = x * y;
        return Fr.unwrap(result);
    }

    // ── UDVT: operator overloading (comparison) ──
    function testFrEq(uint256 a, uint256 b) external pure returns (bool) {
        Fr x = Fr.wrap(a);
        Fr y = Fr.wrap(b);
        return x == y;
    }

    function testFrNeq(uint256 a, uint256 b) external pure returns (bool) {
        Fr x = Fr.wrap(a);
        Fr y = Fr.wrap(b);
        return x != y;
    }

    // ── Enum: member access and comparison ──
    function testEnumValue() external pure returns (uint8) {
        Color c = Color.Blue;
        // Blue is index 2
        return uint8(c);
    }

    function testEnumCompare() external pure returns (bool) {
        return Color.Red != Color.Green;
    }

    function testEnumIndex(uint8 idx) external pure returns (uint8) {
        // Test that enum values map to expected ordinals
        if (idx == 0) return uint8(Color.Red);
        if (idx == 1) return uint8(Color.Green);
        if (idx == 2) return uint8(Color.Blue);
        return uint8(Color.Yellow);
    }

    // ── Unchecked block (just verifying it compiles and runs) ──
    function testUnchecked(uint256 a, uint256 b) external pure returns (uint256) {
        uint256 result;
        unchecked {
            result = a + b;
        }
        return result;
    }
}
