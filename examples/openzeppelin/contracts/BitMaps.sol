// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0) (utils/structs/BitMaps.sol)
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/utils/structs/BitMaps.sol
// MODIFIED — inlined without library/struct wrapper for AVM compilation

pragma solidity ^0.8.20;

/// @dev Implements the BitMaps pattern inline (OZ uses a library+struct wrapper).
/// Each uint256 slot stores 256 booleans as individual bits.
contract BitMapsTest {
    mapping(uint256 bucket => uint256) private _data;

    function get(uint256 index) external view returns (bool) {
        uint256 bucket = index >> 8;
        uint256 mask = 1 << (index & 0xff);
        return _data[bucket] & mask != 0;
    }

    function set(uint256 index) external {
        uint256 bucket = index >> 8;
        uint256 mask = 1 << (index & 0xff);
        _data[bucket] |= mask;
    }

    function unset(uint256 index) external {
        uint256 bucket = index >> 8;
        uint256 mask = 1 << (index & 0xff);
        _data[bucket] &= ~mask;
    }

    function setTo(uint256 index, bool value) external {
        if (value) {
            uint256 bucket = index >> 8;
            uint256 mask = 1 << (index & 0xff);
            _data[bucket] |= mask;
        } else {
            uint256 bucket = index >> 8;
            uint256 mask = 1 << (index & 0xff);
            _data[bucket] &= ~mask;
        }
    }
}
