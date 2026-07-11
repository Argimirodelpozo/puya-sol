// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Hashing: keccak256 / sha256 / ripemd160 over packed + standard encodings.
contract Hash {
    function k256(uint256 x)            external pure returns (uint256) { return uint256(keccak256(abi.encodePacked(x))); }
    function k256two(uint256 a, uint256 b) external pure returns (uint256) { return uint256(keccak256(abi.encodePacked(a, b))); }
    function k256packed(uint64 a, uint64 b) external pure returns (uint256) { return uint256(keccak256(abi.encodePacked(a, b))); }
    function sha(uint256 x)             external pure returns (uint256) { return uint256(sha256(abi.encodePacked(x))); }
    function ripemd(uint256 x)          external pure returns (uint256) { return uint256(uint160(ripemd160(abi.encodePacked(x)))); }
}
