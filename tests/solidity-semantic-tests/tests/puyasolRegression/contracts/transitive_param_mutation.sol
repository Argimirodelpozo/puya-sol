// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the call-graph closure in ParamMutationDetector (possible_solc item
// 3): a param passed on to another callee that mutates it must count as
// mutated — pre-fix, the caller-side write-back was silently dropped.
library LibMut {
    function bumpVia(TransParamMut.S storage q, uint256 v) internal {
        q.f += v;
    }
}

contract TransParamMut {
    struct S { uint256 f; }
    S s;
    using LibMut for S;

    function innerMut(S storage q) internal {
        q.f += 1;
    }

    function outer(S storage p) internal {
        innerMut(p); // no direct mutation of p in THIS body
    }

    function outer2(S storage p) internal {
        outer(p); // two levels deep
    }

    function outerBound(S storage p, uint256 v) internal {
        p.bumpVia(v); // using-for bound hop into the library
    }

    function goStorage() external returns (uint256) {
        s.f = 5;
        outer(s);
        return s.f; // 6
    }

    function goDeep() external returns (uint256) {
        s.f = 10;
        outer2(s);
        return s.f; // 11
    }

    function goBound() external returns (uint256) {
        s.f = 20;
        outerBound(s, 7);
        return s.f; // 27
    }

    function innerMem(uint256[3] memory a) internal pure {
        a[0] = 42;
    }

    function outerMem(uint256[3] memory a) internal pure {
        innerMem(a);
    }

    function goMemory() external pure returns (uint256) {
        uint256[3] memory arr;
        outerMem(arr);
        return arr[0]; // 42
    }
}
