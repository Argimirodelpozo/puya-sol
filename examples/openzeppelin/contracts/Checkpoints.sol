// SPDX-License-Identifier: MIT
// Simplified version of OpenZeppelin Contracts (last updated v5.0.0) (utils/structs/Checkpoints.sol)
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/utils/structs/Checkpoints.sol
// MODIFIED — simplified to mapping-based storage for AVM compilation (no sorted array / binary search)

pragma solidity ^0.8.20;

/// @dev Simplified Checkpoints pattern using mappings.
/// Real OZ uses a sorted array + binary search for gas-efficient historical lookups.
/// We use a mapping since AVM box storage doesn't benefit from the same gas optimizations.
contract CheckpointsTest {
    mapping(uint256 => uint256) private _values;
    uint256 private _latestKey;
    uint256 private _latestValue;
    bool private _hasCheckpoint;

    /// @dev Pushes a (key, value) pair. Keys must be non-decreasing.
    /// Returns (oldValue, newValue).
    function push(uint256 key, uint256 value) external returns (uint256, uint256) {
        uint256 oldValue = _latestValue;
        if (_hasCheckpoint) {
            require(key >= _latestKey, "Checkpoint: decreasing keys");
        }
        _values[key] = value;
        _latestKey = key;
        _latestValue = value;
        _hasCheckpoint = true;
        return (oldValue, value);
    }

    /// @dev Returns the latest stored value.
    function latest() external view returns (uint256) {
        return _latestValue;
    }

    /// @dev Returns whether a checkpoint exists, and if so, the latest key and value.
    function latestCheckpoint() external view returns (bool exists, uint256 key, uint256 value) {
        return (_hasCheckpoint, _latestKey, _latestValue);
    }

    /// @dev Returns the value stored at a given key.
    function getAtKey(uint256 key) external view returns (uint256) {
        return _values[key];
    }

    /// @dev Returns whether any checkpoint has been pushed.
    function hasCheckpoint() external view returns (bool) {
        return _hasCheckpoint;
    }
}
