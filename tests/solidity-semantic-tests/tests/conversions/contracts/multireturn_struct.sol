// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Repro for the puya AWST→IR lowering bug: a multi-return call destructured DIRECTLY
// into struct fields gets the whole call dropped (the materialised return is lost).
// Workaround everywhere: destructure into LOCAL vars first, then assign the fields.
contract MultiReturnStruct {
    struct S4 { uint256 a; uint256 b; uint256 c; uint256 d; }
    struct S2 { uint256 a; uint256 b; }

    function f4(uint256 x) internal pure returns (uint256, uint256, uint256, uint256) {
        return (x + 1, x + 2, x + 3, x + 4);
    }
    function f2(uint256 x) internal pure returns (uint256, uint256) {
        return (x + 10, x + 20);
    }

    function test4(uint256 x) external pure returns (uint256) {
        S4 memory s;
        (s.a, s.b, s.c, s.d) = f4(x);
        return s.a + s.b + s.c + s.d;   // want 4x+10
    }
    function test2(uint256 x) external pure returns (uint256) {
        S2 memory s;
        (s.a, s.b) = f2(x);
        return s.a + s.b;               // want 2x+30
    }
}
