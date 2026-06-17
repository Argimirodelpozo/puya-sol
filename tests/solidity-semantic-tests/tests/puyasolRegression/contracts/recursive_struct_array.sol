// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// A struct that contains a dynamic array of ITSELF (`S[] x` inside S) is recursive;
// puya's IR rejects inline recursive types ("element type does not match array
// type"). The frontend breaks the cycle by mapping the recursive array field's
// element to a fixed projection (struct fields, recursive sub-fields stubbed), so
// puya accepts it and the high-level push/index/field read-write round-trips.
contract C {
    struct S {
        uint16 v;
        S[] x;
    }

    S s;

    constructor() {
        s.v = 21;
        s.x.push();
        s.x.push();
        s.x[0].v = 101;
        s.x[1].v = 102;
    }

    function v0() public returns (uint16) { return s.x[0].v; }
    function v1() public returns (uint16) { return s.x[1].v; }
    function sv() public returns (uint16) { return s.v; }
    function len() public returns (uint256) { return s.x.length; }
}
