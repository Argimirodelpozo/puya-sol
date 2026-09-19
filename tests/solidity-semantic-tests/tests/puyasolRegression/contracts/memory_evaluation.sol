// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;
contract EvaluationAudit {
    function fresh() internal pure returns (uint256[] memory r) {
        assembly { mstore(0x1000, add(mload(0x1000), 1)) }
        r = new uint256[](1); r[0] = 7;
    }
    function indexedOnce() external pure returns (uint256 calls, uint256 value) {
        assembly { mstore(0x1000, 0) }
        value = fresh()[0];
        assembly { calls := mload(0x1000) }
    }
    function assignedOnce() external pure returns (uint256 calls) {
        assembly { mstore(0x1000, 0) }
        fresh()[0] = 9;
        assembly { calls := mload(0x1000) }
    }
    function discardedOnce() external pure returns (uint256 calls) {
        assembly { mstore(0x1000, 0) }
        fresh()[0];
        assembly { calls := mload(0x1000) }
    }
    function lengthControl() external pure returns (uint256 calls, uint256 value) {
        assembly { mstore(0x1000, 0) }
        value = fresh().length;
        assembly { calls := mload(0x1000) }
    }
    function localControl() external pure returns (uint256 calls, uint256 value) {
        assembly { mstore(0x1000, 0) }
        uint256[] memory a = fresh(); value = a[0];
        assembly { calls := mload(0x1000) }
    }
}
