// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// V4 Position shape: a getter on a NESTED mapping field (self.positions) returns
// an element storage ref, mutated via a method. `Outer.S storage self` -> a
// nested `self.inner` mapping -> `Inner.getRef(self.inner, k)` -> `Inner.setN`.
library Inner {
    struct V { uint n; }
    function getRef(mapping(uint => V) storage m, uint k) internal view returns (V storage) {
        return m[k];
    }
    function setN(V storage self, uint v) internal { self.n = v; }
}
library Outer {
    struct S { uint a; mapping(uint => Inner.V) inner; }
    function mutate(S storage self, uint k, uint v) internal {
        Inner.V storage e = Inner.getRef(self.inner, k); // nested-mapping getter -> ref
        Inner.setN(e, v);                                // mutate via the ref
    }
    function read(S storage self, uint k) internal view returns (uint) {
        return self.inner[k].n; // direct nested read (control)
    }
}
contract C {
    mapping(uint => Outer.S) _m;
    function mutate(uint id, uint k, uint v) external { Outer.mutate(_m[id], k, v); }
    function read(uint id, uint k) external view returns (uint) { return Outer.read(_m[id], k); }
}
