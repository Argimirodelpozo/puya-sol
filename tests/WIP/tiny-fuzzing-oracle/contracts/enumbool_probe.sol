// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Enum range/conversion + bool short-circuit/ternary.
contract EnumBool {
    enum E { A, B, C }
    function eRound(uint8 x)  external pure returns (uint256) { return uint256(E(x % 3)); }
    function eFromU(uint8 x)  external pure returns (uint256) { E e = E(x); return uint256(e); }  // x>=3 reverts
    function boolAnd(uint256 a, uint256 b) external pure returns (uint256) { return ((a & 1) == 1) && ((b & 1) == 1) ? 1 : 0; }
    function boolXor(uint256 a, uint256 b) external pure returns (uint256) { return ((a & 1) == 1) != ((b & 1) == 1) ? 1 : 0; }
    function notZero(uint256 a) external pure returns (uint256) { return a != 0 ? 7 : 9; }
}
