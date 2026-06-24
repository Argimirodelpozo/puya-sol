// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Found by the differential fuzzer: checked unsigned ++/++x missed the overflow check
// (only native uint64 reverted, via its opcode; sub-word + biguint silently wrapped).
contract C {
    function postInc8(uint8 a) external pure returns (uint8) { uint8 x=a; x++; return x; }
    function preInc8(uint8 a) external pure returns (uint8) { uint8 x=a; ++x; return x; }
    function postInc16(uint16 a) external pure returns (uint16) { uint16 x=a; x++; return x; }
    function postInc128(uint128 a) external pure returns (uint128) { uint128 x=a; x++; return x; }
    function postInc256(uint256 a) external pure returns (uint256) { uint256 x=a; x++; return x; }
    function uncheckedInc8(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; x++; return x; } }
}
