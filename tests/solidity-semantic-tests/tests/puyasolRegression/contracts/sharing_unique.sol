// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SharingUnique {
    function first(uint256[] memory items) internal pure returns (uint256 result) {
        assembly { result := 0 }
        return items[0];
    }
    function check(uint256 value) external pure returns (uint256) {
        uint256[] memory items = new uint256[](2);
        items[0] = value;
        return first(items);
    }
}
