// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Edges {
    enum Color { Red, Green, Blue }
    function sha(bytes calldata b) external pure returns (bytes32) { return sha256(b); }
    function ripe(bytes calldata b) external pure returns (bytes20) { return ripemd160(b); }
    function kec(bytes calldata b) external pure returns (bytes32) { return keccak256(b); }
    function recover(bytes32 h, uint8 v, bytes32 r, bytes32 s) external pure returns (address) {
        return ecrecover(h, v, r, s);
    }
    function enumFromInt(uint8 x) external pure returns (Color) { return Color(x); }   // invalid x -> panic
    function enumToInt(Color c) external pure returns (uint8) { return uint8(c); }
    function divZero(uint256 a, uint256 b) external pure returns (uint256) { return a / b; }   // b=0 -> revert
    function modZero(uint256 a, uint256 b) external pure returns (uint256) { return a % b; }
    function typeMax() external pure returns (uint256, int256, uint8) {
        return (type(uint256).max, type(int256).min, type(uint8).max);
    }
    function arrOOB(uint256 i) external pure returns (uint256) {
        uint256[3] memory a = [uint256(10), 20, 30]; return a[i];   // i>=3 -> panic
    }
}
