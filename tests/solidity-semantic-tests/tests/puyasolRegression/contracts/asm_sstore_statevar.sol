// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Assembly sstore/sload on a scalar app-global state var route to v's OWN storage
// (unifying the high-level box/global model with assembly), not the disjoint
// __dyn_storage blob. Direct .slot reference only.
contract C {
    uint256 v;
    function f() public returns (uint256) {
        assembly { sstore(v.slot, 42) }
        return v; // high-level read sees the asm write
    }
    function g() public returns (uint256) {
        assembly { sstore(v.slot, 99) }
        uint256 r;
        assembly { r := sload(v.slot) }
        return r; // asm read sees the asm write (both routed)
    }
}
