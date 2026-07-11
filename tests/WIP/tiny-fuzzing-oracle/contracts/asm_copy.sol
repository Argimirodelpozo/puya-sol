// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // calldatacopy first word of a bytes arg into memory
    function cdcopy(bytes calldata x) external pure returns (uint256 r) {
        assembly { calldatacopy(0, x.offset, 32) r := mload(0) }
    }
    // calldataload at a let-local offset derived from x.offset
    function cdload(bytes calldata x) external pure returns (uint256 r) {
        assembly { let o := x.offset r := calldataload(o) }
    }
    // calldatacopy a fixed slice into memory then read shifted
    function cdcopy2(uint256 a, bytes calldata x) external pure returns (uint256 r) {
        assembly { calldatacopy(0, x.offset, 32) r := mload(0) }
    }
}
