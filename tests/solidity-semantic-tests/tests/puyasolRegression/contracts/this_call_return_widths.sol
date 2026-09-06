// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// `this.f()` calls the ABI method itself, whose return travels in the WIRE
// shape (arc4.uint8 / arc4.uint128 / arc4 tuples); the caller must decode it
// back to the native values the surrounding code computes with.
contract ThisCallReturnWidths {
    uint128 public w = 5;
    uint8 public b = 7;
    struct S { uint128 a; uint32 c; }
    S public s = S(1, 2);
    function sub() external pure returns (uint16) { return 65535; }
    function pair() external pure returns (uint8, uint128) { return (3, 4); }

    function wide() external view returns (uint256) { return uint256(this.w()) + 1; }
    function narrow() external view returns (uint256) { return uint256(this.b()) + 1; }
    function structGetter() external view returns (uint256) { (uint128 a, uint32 c) = this.s(); return uint256(a) + c; }
    function explicitSub() external view returns (uint256) { return uint256(this.sub()) + 1; }
    function tuple() external view returns (uint256) { (uint8 x, uint128 y) = this.pair(); return uint256(x) * 10 + y; }
}
