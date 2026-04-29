// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibSort.sol";

contract LibSortWrapper {
    function sort(uint256[] calldata a) external pure returns (uint256[] memory) {
        uint256[] memory b = a;
        LibSort.sort(b);
        return b;
    }

    function isSorted(uint256[] calldata a) external pure returns (bool) {
        uint256[] memory b = a;
        return LibSort.isSorted(b);
    }

    function uniquifySorted(uint256[] calldata a) external pure returns (uint256[] memory) {
        uint256[] memory b = a;
        LibSort.sort(b);
        LibSort.uniquifySorted(b);
        return b;
    }
}
