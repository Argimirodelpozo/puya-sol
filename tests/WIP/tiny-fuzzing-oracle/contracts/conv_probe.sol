// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Type-conversion matrix: truncation direction, sign-extension, reinterpret, chained narrowing.
contract Conv {
    function u256toi8(uint256 x)  external pure returns (int8)    { return int8(uint8(x)); }
    function i256toi8(int256 x)   external pure returns (int8)    { return int8(x); }
    function i8toi256(int256 x)   external pure returns (int256)  { return int256(int8(int256(x))); }
    function uToI(uint256 x)      external pure returns (int256)  { return int256(x); }
    function iToU(int256 x)       external pure returns (uint256) { return uint256(x); }
    function uToB32ToU(uint256 x) external pure returns (uint256) { return uint256(bytes32(x)); }
    function uToAddrToU(uint256 x)external pure returns (uint256) { return uint256(uint160(x)); }
    function narrowChain(uint256 x) external pure returns (uint256) {
        return uint256(uint8(uint16(uint32(uint64(x)))));
    }
    function sxTrunc(int256 x)    external pure returns (int256)  { return int256(int16(int8(x))); }
    function widenNarrow(int256 x)external pure returns (int256)  { return int8(int40(x)); }
}
