// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;
import {LibString} from "./LibString.sol";
// Wrapper exposing Solady LibString pure functions for differential fuzzing.
contract LibStringWrapper {
    function toStringU(uint256 v) external pure returns (string memory) { return LibString.toString(v); }
    function toStringI(int256 v) external pure returns (string memory) { return LibString.toString(v); }
    function toHex(uint256 v) external pure returns (string memory) { return LibString.toHexString(v); }
    function toHexNoPrefix(uint256 v) external pure returns (string memory) { return LibString.toHexStringNoPrefix(v); }
    function runeCount(string calldata s) external pure returns (uint256) { return LibString.runeCount(s); }
    function is7BitASCII(string calldata s) external pure returns (bool) { return LibString.is7BitASCII(s); }
    function indexOf(string calldata a, string calldata b) external pure returns (uint256) { return LibString.indexOf(a, b); }
    function contains(string calldata a, string calldata b) external pure returns (bool) { return LibString.contains(a, b); }
    function startsWith(string calldata a, string calldata b) external pure returns (bool) { return LibString.startsWith(a, b); }
    function endsWith(string calldata a, string calldata b) external pure returns (bool) { return LibString.endsWith(a, b); }
    function repeat(string calldata s, uint256 n) external pure returns (string memory) { return LibString.repeat(s, n); }
    function slice(string calldata s, uint256 start) external pure returns (string memory) { return LibString.slice(s, start); }
    function concat(string calldata a, string calldata b) external pure returns (string memory) { return LibString.concat(a, b); }
    function lower(string calldata s) external pure returns (string memory) { return LibString.lower(s); }
    function upper(string calldata s) external pure returns (string memory) { return LibString.upper(s); }
    function eq(string calldata a, string calldata b) external pure returns (bool) { return LibString.eq(a, b); }
    function cmp(string calldata a, string calldata b) external pure returns (int256) { return LibString.cmp(a, b); }
    function escapeHTML(string calldata s) external pure returns (string memory) { return LibString.escapeHTML(s); }
    function escapeJSON(string calldata s) external pure returns (string memory) { return LibString.escapeJSON(s); }
}
