// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract EnumerableMapTest {
    mapping(uint256 => uint256) private _values;
    mapping(uint256 => uint256) private _indexOf;
    mapping(uint256 => uint256) private _keyAtIndex;
    uint256 private _length;

    function set(uint256 key, uint256 value) external returns (bool) {
        _values[key] = value;
        if (_indexOf[key] == 0) {
            _length += 1;
            _indexOf[key] = _length;
            _keyAtIndex[_length] = key;
            return true; // new entry
        }
        return false; // updated existing
    }

    function remove(uint256 key) external returns (bool) {
        uint256 keyIndex = _indexOf[key];
        if (keyIndex == 0) return false;

        uint256 lastIndex = _length;
        if (keyIndex != lastIndex) {
            uint256 lastKey = _keyAtIndex[lastIndex];
            _keyAtIndex[keyIndex] = lastKey;
            _indexOf[lastKey] = keyIndex;
        }

        delete _keyAtIndex[lastIndex];
        delete _indexOf[key];
        delete _values[key];
        _length -= 1;
        return true;
    }

    function contains(uint256 key) external view returns (bool) {
        return _indexOf[key] != 0;
    }

    function length() external view returns (uint256) {
        return _length;
    }

    function at(uint256 index) external view returns (uint256 key, uint256 value) {
        require(index > 0 && index <= _length, "index out of bounds");
        key = _keyAtIndex[index];
        value = _values[key];
    }

    function get(uint256 key) external view returns (uint256) {
        require(_indexOf[key] != 0, "key not found");
        return _values[key];
    }

    function tryGet(uint256 key) external view returns (bool exists, uint256 value) {
        exists = _indexOf[key] != 0;
        value = _values[key]; // 0 if key not found
    }
}
