// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibString.sol";

contract LibStringWrapper {
    function toString(uint256 value) external pure returns (string memory) {
        return LibString.toString(value);
    }

    function toHexString(uint256 value) external pure returns (string memory) {
        return LibString.toHexString(value);
    }

    function toHexStringNoPrefix(uint256 value) external pure returns (string memory) {
        return LibString.toHexStringNoPrefix(value);
    }

    function toHexStringChecksummed(address value) external pure returns (string memory) {
        return LibString.toHexStringChecksummed(value);
    }

    function toMinimalHexString(uint256 value) external pure returns (string memory) {
        return LibString.toMinimalHexString(value);
    }

    function runeCount(string calldata s) external pure returns (uint256) {
        return LibString.runeCount(s);
    }

    function eq(string calldata a, string calldata b) external pure returns (bool) {
        return LibString.eq(a, b);
    }

    function replace(string calldata subject, string calldata search, string calldata replacement) external pure returns (string memory) {
        return LibString.replace(subject, search, replacement);
    }

    function indexOf(string calldata subject, string calldata search) external pure returns (uint256) {
        return LibString.indexOf(subject, search);
    }

    function repeat(string calldata subject, uint256 times) external pure returns (string memory) {
        return LibString.repeat(subject, times);
    }

    function slice(string calldata subject, uint256 start, uint256 end) external pure returns (string memory) {
        return LibString.slice(subject, start, end);
    }

    function upper(string calldata subject) external pure returns (string memory) {
        return LibString.upper(subject);
    }

    function lower(string calldata subject) external pure returns (string memory) {
        return LibString.lower(subject);
    }

    function contains(string calldata subject, string calldata search) external pure returns (bool) {
        return LibString.contains(subject, search);
    }

    function startsWith(string calldata subject, string calldata search) external pure returns (bool) {
        return LibString.startsWith(subject, search);
    }

    function endsWith(string calldata subject, string calldata search) external pure returns (bool) {
        return LibString.endsWith(subject, search);
    }

    function escapeHTML(string calldata subject) external pure returns (string memory) {
        return LibString.escapeHTML(subject);
    }

    function escapeJSON(string calldata subject) external pure returns (string memory) {
        return LibString.escapeJSON(subject);
    }

    function packOne(string calldata a) external pure returns (bytes32) {
        return LibString.packOne(a);
    }

    function unpackOne(bytes32 packed) external pure returns (string memory) {
        return LibString.unpackOne(packed);
    }
}
