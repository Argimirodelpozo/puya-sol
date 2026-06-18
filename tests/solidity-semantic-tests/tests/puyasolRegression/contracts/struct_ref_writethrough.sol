// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test.
// A SMALL (app-global) struct passed by reference to an internal CONTRACT METHOD must write
// through to the caller's storage. Pre handle-model Stage 1b this hit copy+write-back (which
// doesn't reach contract methods) → the write was a dead local store puya DCE'd → returned 0.
// Now a struct that is passed by ref to a contract method is boxed on demand (targeted), so
// the ref travels as a box-key handle that writes through. (11cb361306)
contract StructRefWriteThrough {
    struct S { uint256 x; uint256 y; }
    S single;

    function f() external returns (uint256) {
        single.x = 0;
        _bump(single);       // pass the small struct state var by ref to a contract method
        return single.x;     // EVM: 5
    }
    function _bump(S storage s) internal { s.x = 5; }
}
