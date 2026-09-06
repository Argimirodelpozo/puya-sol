// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract HugeSparseArray {
    uint256[2**200] public values;
    function set(uint256 index, uint256 value) external { values[index] = value; }
}
