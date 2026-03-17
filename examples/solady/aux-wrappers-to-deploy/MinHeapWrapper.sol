// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/MinHeapLib.sol";

contract MinHeapWrapper {
    using MinHeapLib for MinHeapLib.Heap;

    MinHeapLib.Heap private heap;

    function push(uint256 value) external {
        heap.push(value);
    }

    function pop() external returns (uint256) {
        return heap.pop();
    }

    function root() external view returns (uint256) {
        return heap.root();
    }

    function length() external view returns (uint256) {
        return heap.length();
    }
}
