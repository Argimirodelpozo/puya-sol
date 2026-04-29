// SPDX-License-Identifier: MIT
// Inspired by OpenZeppelin DoubleEndedQueue.sol
// Simplified to a stack (LIFO) to avoid biguint key normalization issues.
// Demonstrates: mapping as sparse array, custom errors, uint256 state tracking
pragma solidity ^0.8.20;

contract DequeTest {
    mapping(uint256 => uint256) private _data;
    uint256 private _size;

    error StackEmpty();
    error StackOutOfBounds();

    function push(uint256 value) external {
        uint256 idx = _size;
        _size = idx + 1;
        _data[idx] = value;
    }

    function pop() external returns (uint256) {
        if (_size == 0) revert StackEmpty();
        uint256 idx = _size - 1;
        _size = idx;
        uint256 value = _data[idx];
        return value;
    }

    function top() external view returns (uint256) {
        if (_size == 0) revert StackEmpty();
        return _data[_size - 1];
    }

    function at(uint256 index) external view returns (uint256) {
        if (index >= _size) revert StackOutOfBounds();
        return _data[index];
    }

    function length() external view returns (uint256) {
        return _size;
    }

    function empty() external view returns (bool) {
        return _size == 0;
    }
}
