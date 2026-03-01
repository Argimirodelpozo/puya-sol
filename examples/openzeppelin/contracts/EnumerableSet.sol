// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract EnumerableSetTest {
    // Simplified enumerable set using mappings
    // Maps value -> exists flag, plus a length counter
    mapping(uint256 => bool) private _contains;
    uint256 private _length;

    function add(uint256 value) external returns (bool) {
        if (_contains[value]) return false;
        _contains[value] = true;
        _length += 1;
        return true;
    }

    function remove(uint256 value) external returns (bool) {
        if (!_contains[value]) return false;
        _contains[value] = false;
        _length -= 1;
        return true;
    }

    function contains(uint256 value) external view returns (bool) {
        return _contains[value];
    }

    function length() external view returns (uint256) {
        return _length;
    }
}
