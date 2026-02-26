// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

contract HashTest {
    function hashSha256(bytes memory data) external pure returns (bytes32) {
        return sha256(data);
    }

    function hashCombined(uint256 a, uint256 b) external pure returns (bytes32) {
        return sha256(abi.encodePacked(a, b));
    }

    function hashKeccak(bytes memory data) external pure returns (bytes32) {
        return keccak256(data);
    }
}
