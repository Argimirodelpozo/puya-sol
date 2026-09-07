// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StorageShapes {
    struct Counter { uint256 value; }
    struct Checkpoint { uint32 fromBlock; uint224 votes; }   // packed, one slot

    mapping(address => uint256) public bal;                  // scalar
    mapping(address => mapping(address => uint256)) public allow;  // nested
    mapping(address => Counter) public nonces;               // struct
    mapping(address => Checkpoint[]) public ckpts;           // dynamic array
    uint256 public total;
    // rev-2 holder format 2 shapes: a struct value that itself holds a
    // mapping, and an EnumerableSet-style transparent wrapper inside a map.
    struct Account { uint64 tag; mapping(address => uint256) sub; }
    struct Set { bytes32[] _values; mapping(bytes32 => uint256) _positions; }
    struct AddressSet { Set _inner; }
    mapping(address => Account) internal accts;              // struct WITH mapping
    mapping(uint256 => AddressSet) internal members;         // transparent wrapper
    AddressSet internal topSet;                              // struct ROOT holding a mapping

    function credit(address a, uint256 v) external { bal[a] += v; total += v; }
    function approve(address o, address s, uint256 v) external { allow[o][s] = v; }
    function bump(address a) external { nonces[a].value += 1; }
    function push(address a, uint32 fb, uint224 v) external {
        ckpts[a].push(Checkpoint(fb, v));
    }
    function ckptLen(address a) external view returns (uint256) {
        return ckpts[a].length;
    }
    function tag(address a, uint64 t, address s, uint256 v) external {
        accts[a].tag = t;
        accts[a].sub[s] = v;
    }
    function join(uint256 g, address a) external {
        AddressSet storage set = members[g];
        bytes32 k = bytes32(uint256(uint160(a)));
        if (set._inner._positions[k] == 0) {
            set._inner._values.push(k);
            set._inner._positions[k] = set._inner._values.length;
        }
    }
    function joinTop(address a) external {
        bytes32 k = bytes32(uint256(uint160(a)));
        if (topSet._inner._positions[k] == 0) {
            topSet._inner._values.push(k);
            topSet._inner._positions[k] = topSet._inner._values.length;
        }
    }
    function topCount() external view returns (uint256) { return topSet._inner._values.length; }
    function memberCount(uint256 g) external view returns (uint256) {
        return members[g]._inner._values.length;
    }
    function acctTag(address a) external view returns (uint64) {
        return accts[a].tag;
    }
}
