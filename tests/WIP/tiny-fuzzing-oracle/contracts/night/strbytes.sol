// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract StrBytes {
    function strlen(string calldata s) external pure returns (uint256) { return bytes(s).length; }
    function byteAt(bytes calldata b, uint256 i) external pure returns (bytes1) { return b[i]; }
    function concat(string calldata a, string calldata b) external pure returns (uint256) {
        return bytes(string.concat(a, b)).length;
    }
    function eq(string calldata a, string calldata b) external pure returns (bool) {
        return keccak256(bytes(a)) == keccak256(bytes(b));
    }
    function slice(bytes calldata b) external pure returns (uint256) { return b.length; }
    function toBytes(uint256 x) external pure returns (uint256) { return abi.encodePacked(x).length; }
    function cmp(bytes32 a, bytes32 b) external pure returns (bool) { return a == b; }
    function bytesEq(bytes calldata a, bytes calldata b) external pure returns (bool) {
        return keccak256(a) == keccak256(b);
    }
}
