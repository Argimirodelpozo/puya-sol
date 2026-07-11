// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract MemParamNested {
    struct Inner { uint256 a; }
    struct Outer { Inner inner; uint256 b; }
    struct S { uint256 x; }
    function _mutOuter(Outer memory o) internal pure { o.inner.a = 11; o.b = 22; }
    function _mut2(S memory a, S memory b) internal pure { a.x = 11; b.x = 22; }
    function nestedStruct() external pure returns (uint256) {
        Outer memory o; o.inner.a = 1; o.b = 2;
        _mutOuter(o); return o.inner.a + o.b;          // EVM: 33
    }
    function twoStructParams() external pure returns (uint256) {
        S memory s1 = S(1); S memory s2 = S(2);
        _mut2(s1, s2); return s1.x + s2.x;              // EVM: 33
    }
}
