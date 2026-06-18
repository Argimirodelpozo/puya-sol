// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test.
// A storage struct-array passed BY REFERENCE to an internal (contract-method) function must
// write through to the caller's storage. Pre handle-model this hit copy+write-back, which
// doesn't reach contract methods, so the write was a dead local store puya DCE'd → returned 0.
// Now the array ref travels as a box-key handle and a[i].field=v emits box_replace at the
// ARC4 offset, writing through. (handle model Stage 1a-arrays, d43f316001)
contract ArrayRefWriteThrough {
    struct P { uint256 x; uint256 y; }
    P[] arr;

    function f() external returns (uint256) {
        delete arr;
        arr.push(P(0, 0));
        _bump(arr);          // pass the storage array ref to a contract method
        return arr[0].x;     // EVM: 5
    }
    function _bump(P[] storage a) internal { a[0].x = 5; }
}
