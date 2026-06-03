// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Regression guard for the struct-storage-ref model (Uniswap V4 Pool.State shape):
// `mapping(K => Struct-with-nested-mapping)` accessed through storage references.
// Specifically covers a LOCAL storage-ref variable bound from a getter that
// returns the mapping element — the V4 `Pool.State storage pool = _getPool(id);
// pool.checkPoolInitialized(); pool.modifyLiquidity(...)` pattern. The local must
// resolve to the element's runtime box key (sha256(id ++ "_m")), NOT its own
// variable name, so it reads/writes the SAME box a direct `_m[id]` access does.
library L {
    struct S { uint a; uint b; mapping(uint => uint) inner; }

    function bump(S storage self, uint k, uint v) internal {
        self.a = self.a + 1;
        self.b = v;
        self.inner[k] = v;
    }
    function get(S storage self, uint k) internal view returns (uint, uint, uint) {
        return (self.a, self.b, self.inner[k]);
    }
    // mirrors Pool.checkPoolInitialized: reverts when the element is uninitialised
    function checkInit(S storage self) internal view {
        require(self.a != 0, "NOT_INIT");
    }
}

contract StructStorageRefLocal {
    using L for L.S;
    mapping(uint => L.S) _m;

    function _getS(uint id) internal view returns (L.S storage) { return _m[id]; }

    // CONTROL: direct index access (the already-verified path)
    function bumpDirect(uint id, uint k, uint v) external { _m[id].bump(k, v); }
    function getDirect(uint id, uint k) external view returns (uint, uint, uint) {
        return _m[id].get(k);
    }

    // THE FIX: a local storage-ref var bound from a getter, then method calls
    function bumpLocal(uint id, uint k, uint v) external {
        L.S storage s = _getS(id);
        s.checkInit();      // like pool.checkPoolInitialized()
        s.bump(k, v);       // like pool.modifyLiquidity(...)
    }
    function getLocal(uint id, uint k) external view returns (uint, uint, uint) {
        L.S storage s = _getS(id);
        return s.get(k);
    }
}
