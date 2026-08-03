// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// OZ EnumerableSet.AddressSet shape, reduced to the two things that broke it:
//   * a dynamic array living in a struct that ALSO holds a mapping, and
//   * `address[] memory result; assembly { result := store }` — the pointer pun
//     written to an UNINITIALISED local (the named-return spelling always worked).
// Together these made values() return [] for a non-empty set while length(),
// at(i) and contains() all answered correctly.

library ESet {
    struct Set { bytes32[] _values; mapping(bytes32 => uint256) _indexes; }
    struct AddressSet { Set _inner; }

    function _add(Set storage set, bytes32 v) private returns (bool) {
        if (set._indexes[v] != 0) return false;
        set._values.push(v);
        set._indexes[v] = set._values.length;
        return true;
    }
    function _values(Set storage set) private view returns (bytes32[] memory) {
        return set._values;
    }
    function _length(Set storage set) private view returns (uint256) {
        return set._values.length;
    }
    function _at(Set storage set, uint256 i) private view returns (bytes32) {
        return set._values[i];
    }

    function add(AddressSet storage set, address v) internal returns (bool) {
        return _add(set._inner, bytes32(uint256(uint160(v))));
    }
    function length(AddressSet storage set) internal view returns (uint256) {
        return _length(set._inner);
    }
    function at(AddressSet storage set, uint256 i) internal view returns (address) {
        return address(uint160(uint256(_at(set._inner, i))));
    }
    function values(AddressSet storage set) internal view returns (address[] memory) {
        bytes32[] memory store = _values(set._inner);
        address[] memory result;
        assembly { result := store }
        return result;
    }
}

contract EnumerableSetValues {
    using ESet for ESet.AddressSet;
    ESet.AddressSet internal _set;

    function add(address a) external returns (bool) { return _set.add(a); }
    function length() external view returns (uint256) { return _set.length(); }
    function at(uint256 i) external view returns (address) { return _set.at(i); }
    function values() external view returns (address[] memory) { return _set.values(); }
}
