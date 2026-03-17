// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Basic types regression test.
/// Tests uint256, uint64, bool, address, bytes32, string, int256.
contract BasicTypes {
    uint256 public storedUint;
    bool public storedBool;
    address public storedAddr;
    bytes32 public storedBytes32;
    int256 public storedInt;

    function setUint(uint256 val) external { storedUint = val; }
    function getUint() external view returns (uint256) { return storedUint; }

    function setBool(bool val) external { storedBool = val; }
    function getBool() external view returns (bool) { return storedBool; }

    function setAddr(address val) external { storedAddr = val; }
    function getAddr() external view returns (address) { return storedAddr; }

    function setBytes32(bytes32 val) external { storedBytes32 = val; }
    function getBytes32() external view returns (bytes32) { return storedBytes32; }

    function setInt(int256 val) external { storedInt = val; }
    function getInt() external view returns (int256) { return storedInt; }

    // Pure functions for type operations
    function addUints(uint256 a, uint256 b) external pure returns (uint256) {
        return a + b;
    }

    function boolAnd(bool a, bool b) external pure returns (bool) {
        return a && b;
    }

    function boolOr(bool a, bool b) external pure returns (bool) {
        return a || b;
    }

    function boolNot(bool a) external pure returns (bool) {
        return !a;
    }

    function identityAddress(address a) external pure returns (address) {
        return a;
    }

    function identityBytes32(bytes32 a) external pure returns (bytes32) {
        return a;
    }
}
