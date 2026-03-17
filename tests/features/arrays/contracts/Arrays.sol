// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Array operations regression test.
contract Arrays {
    // Fixed-size memory arrays
    function sumFixedArray() external pure returns (uint256) {
        uint256[5] memory arr = [uint256(1), 2, 3, 4, 5];
        uint256 total = 0;
        for (uint256 i = 0; i < 5; i++) {
            total += arr[i];
        }
        return total;
    }

    function getElement(uint256 index) external pure returns (uint256) {
        uint256[3] memory arr = [uint256(10), 20, 30];
        return arr[index];
    }

    function setElement() external pure returns (uint256) {
        uint256[3] memory arr = [uint256(0), 0, 0];
        arr[1] = 42;
        return arr[1];
    }

    // Dynamic memory array
    function createDynamic(uint256 n) external pure returns (uint256) {
        uint256[] memory arr = new uint256[](n);
        for (uint256 i = 0; i < n; i++) {
            arr[i] = i * 10;
        }
        return arr[n - 1];
    }

    // Array length
    function fixedLength() external pure returns (uint256) {
        uint256[7] memory arr;
        return arr.length;
    }

    // Return array
    function returnArray() external pure returns (uint256[3] memory) {
        return [uint256(100), 200, 300];
    }
}
