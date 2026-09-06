// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// OpenZeppelin shapes: a library takes `Trace storage` / `Map storage` and
// hands an INTERIOR part of it to another library function by reference —
// `self._checkpoints` (dynamic array) and `map._keys` (a struct member that
// holds a mapping). Default mode specializes the callee on the field path
// under the enclosing box, so the callee's writes land where direct access
// reads them.
library Checkpoints {
    struct Checkpoint { uint32 _key; uint224 _value; }
    struct Trace { Checkpoint[] _checkpoints; }

    function push(Trace storage self, uint32 key, uint224 value) internal returns (uint224, uint224) {
        return _insert(self._checkpoints, key, value);
    }
    function latest(Trace storage self) internal view returns (uint224) {
        uint256 pos = self._checkpoints.length;
        return pos == 0 ? 0 : self._checkpoints[pos - 1]._value;
    }
    function length(Trace storage self) internal view returns (uint256) {
        return self._checkpoints.length;
    }
    function _insert(Checkpoint[] storage self, uint32 key, uint224 value) private returns (uint224, uint224) {
        uint256 pos = self.length;
        if (pos > 0) {
            Checkpoint storage last = self[pos - 1];
            require(last._key <= key, "unordered");
            if (last._key == key) {
                last._value = value;
            } else {
                self.push(Checkpoint({_key: key, _value: value}));
            }
            return (last._value, value);
        }
        self.push(Checkpoint({_key: key, _value: value}));
        return (0, value);
    }
}

library EnumerableSet {
    struct Set { bytes32[] _values; mapping(bytes32 => uint256) _positions; }
    function add(Set storage set, bytes32 value) internal returns (bool) {
        if (!contains(set, value)) {
            set._values.push(value);
            set._positions[value] = set._values.length;
            return true;
        }
        return false;
    }
    function contains(Set storage set, bytes32 value) internal view returns (bool) {
        return set._positions[value] != 0;
    }
    function length(Set storage set) internal view returns (uint256) { return set._values.length; }
}

library EnumerableMap {
    using EnumerableSet for EnumerableSet.Set;
    struct Bytes32ToUintMap { EnumerableSet.Set _keys; mapping(bytes32 => uint256) _values; }
    function set(Bytes32ToUintMap storage map, bytes32 key, uint256 value) internal returns (bool) {
        map._values[key] = value;
        return map._keys.add(key);
    }
    function get(Bytes32ToUintMap storage map, bytes32 key) internal view returns (uint256) {
        require(map._keys.contains(key), "missing");
        return map._values[key];
    }
    function length(Bytes32ToUintMap storage map) internal view returns (uint256) { return map._keys.length(); }
}

contract InteriorStorageRefPaths {
    using Checkpoints for Checkpoints.Trace;
    using EnumerableMap for EnumerableMap.Bytes32ToUintMap;

    mapping(uint256 => Checkpoints.Trace) private _traces;
    Checkpoints.Trace private _total;
    EnumerableMap.Bytes32ToUintMap private _map;
    mapping(uint256 => EnumerableMap.Bytes32ToUintMap) private _maps;

    function push(uint256 id, uint32 key, uint224 value) external returns (uint224 old, uint224 cur) {
        (old, cur) = _traces[id].push(key, value);
        _total.push(key, value * 2);
    }
    function latest(uint256 id) external view returns (uint224) { return _traces[id].latest(); }
    function len(uint256 id) external view returns (uint256) { return _traces[id].length(); }
    function total() external view returns (uint224) { return _total.latest(); }
    function totalLen() external view returns (uint256) { return _total.length(); }

    function put(bytes32 k, uint256 v) external returns (bool) { return _map.set(k, v); }
    function get(bytes32 k) external view returns (uint256) { return _map.get(k); }
    function size() external view returns (uint256) { return _map.length(); }
    function putIn(uint256 id, bytes32 k, uint256 v) external returns (bool) { return _maps[id].set(k, v); }
    function sizeIn(uint256 id) external view returns (uint256) { return _maps[id].length(); }
    // Direct interior access must agree with the library's view of the same data.
    function directSize() external view returns (uint256) { return _map._keys._values.length; }
}
