// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test.
// Dual (key,offset) struct-ref handle: _bs's param `s` receives an array element (arr[0]), a
// whole-box state struct (single), AND a mapping value (m[1]) — all must write through to x=5.
contract StructElemRef {
    struct S { uint256 x; uint256 y; }
    S[] arr;
    S single;
    mapping(uint256 => S) m;
    function _bs(S storage s) internal { s.x = 5; }
    function arrayElem() external returns (uint256) {
        delete arr; arr.push(S(0, 0)); _bs(arr[0]); return arr[0].x;   // 5 (element slice)
    }
    function structVar() external returns (uint256) {
        single.x = 0; _bs(single); return single.x;                   // 5 (whole box, offset 0)
    }
    function mapVal() external returns (uint256) {
        m[1].x = 0; _bs(m[1]); return m[1].x;                         // 5 (mapping value box)
    }
}
