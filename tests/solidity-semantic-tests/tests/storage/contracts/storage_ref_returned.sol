// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Regression guard: a storage reference RETURNED FROM A FUNCTION must stay a ref,
// so a mutation through it persists. Mirrors Uniswap V4 Position:
//   Position.State storage position = self.positions.get(...);  // ref via a library getter
//   position.update(...);                                       // mutates via the ref
// The getter takes a `mapping(...) storage` PARAM and returns an element `T storage`.
// Before the fix the return was lowered to the element's VALUE (a copy), so the
// mutation was discarded (no box write-back) and a later read saw the old data.
library L {
    struct S { uint a; mapping(uint => uint) inner; }

    // getter on a MAPPING PARAM, returning an element storage ref (the Position.get shape)
    function getRef(mapping(uint => S) storage m, uint k) internal view returns (S storage) {
        return m[k];
    }
    // mutate through a storage ref (the Position.update shape)
    function bump(S storage self, uint ik, uint v) internal {
        self.a = self.a + 1;
        self.inner[ik] = v;
    }
    function rd(S storage self, uint ik) internal view returns (uint, uint) {
        return (self.a, self.inner[ik]);
    }
}

contract StorageRefReturned {
    using L for L.S;
    mapping(uint => L.S) _m;

    // CONTROL: direct index access (already-verified path)
    function bumpDirect(uint id, uint ik, uint v) external { _m[id].bump(ik, v); }
    function rdDirect(uint id, uint ik) external view returns (uint, uint) {
        return _m[id].rd(ik);
    }

    // THE FIX: mutate through a ref RETURNED from the param-mapping getter
    function bumpViaRef(uint id, uint ik, uint v) external {
        L.S storage s = L.getRef(_m, id);
        s.bump(ik, v);
    }
    // read through a ref returned from the getter (also chained form)
    function rdViaRef(uint id, uint ik) external view returns (uint, uint) {
        return L.getRef(_m, id).rd(ik);
    }
}
