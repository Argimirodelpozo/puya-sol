// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract OffsetStructPin {
    struct S { uint64 f; uint64 g; }
    S[10] arr;

    function bump(S storage s) internal returns (uint64) {
        s.g += 1;
        return 2;
    }
    function inner(S storage s) internal {
        s.f += bump(s);
    }
    function probe(uint256 i) external returns (uint64 f, uint64 g, uint64 g0) {
        arr[i].f = 10;
        inner(arr[i]);
        f = arr[i].f;
        g = arr[i].g;
        g0 = arr[0].g;
    }
}
