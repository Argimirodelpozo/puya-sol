// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Fixed-bytes shift/bitwise/compare (built from int inputs so the fuzzer can drive them).
contract FixedBytes {
    function shlB(uint256 x, uint8 n)  external pure returns (uint256) { return uint256(bytes32(x) << n); }
    function shrB(uint256 x, uint8 n)  external pure returns (uint256) { return uint256(bytes32(x) >> n); }
    function andB(uint256 a, uint256 b) external pure returns (uint256) { return uint256(bytes32(a) & bytes32(b)); }
    function orB(uint256 a, uint256 b) external pure returns (uint256) { return uint256(bytes32(a) | bytes32(b)); }
    function notB(uint256 a)           external pure returns (uint256) { return uint256(~bytes32(a)); }
    function idxB(uint256 x, uint8 i)  external pure returns (uint8)   { return uint8(bytes32(x)[i % 32]); }
    function truncShift(uint256 x, uint8 n) external pure returns (uint32) { return uint32(bytes4(bytes32(x) << n)); }
}
