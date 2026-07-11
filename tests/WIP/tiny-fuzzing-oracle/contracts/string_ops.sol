// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function cat2(string calldata a, string calldata b) external pure returns (string memory) {
        return string.concat(a, b);
    }
    function catLen(bytes calldata a, bytes calldata b) external pure returns (uint256) {
        return bytes.concat(a, b).length;
    }
    function eq(string calldata a, string calldata b) external pure returns (bool) {
        return keccak256(bytes(a)) == keccak256(bytes(b));
    }
    function byteAt(bytes calldata a, uint256 i) external pure returns (bytes1) {
        return a[i % (a.length == 0 ? 1 : a.length)];
    }
    function catThree(string calldata a) external pure returns (uint256) {
        return bytes(string.concat(a, "-", a)).length;
    }
    function sliceLen(bytes calldata a, uint256 n) external pure returns (uint256) {
        uint256 k = n % (a.length + 1);
        bytes calldata s = a[0:k];
        return s.length;
    }
}
