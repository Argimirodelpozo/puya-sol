// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/DynamicArrayLib.sol";

contract DynamicArrayWrapper {
    function testSlice(uint256[] calldata a, uint256 start, uint256 end) external pure returns (uint256[] memory) {
        return DynamicArrayLib.slice(a, start, end);
    }

    function testCopy(uint256[] calldata a) external pure returns (uint256[] memory) {
        return DynamicArrayLib.copy(a);
    }
}
