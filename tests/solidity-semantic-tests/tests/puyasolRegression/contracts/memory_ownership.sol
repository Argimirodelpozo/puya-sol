// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;
contract OwnershipAudit {
    struct S { uint256[] a; }
    struct Pair { S left; S right; }
    function wrap(uint256[] memory a) internal pure returns (S memory) { return S(a); }
    function identity(uint256[] memory a) internal pure returns (uint256[] memory) { return a; }
    function freshStruct() external pure returns (uint256, uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        S memory s = S(p); s.a[0] = 9;
        return (p[0], s.a[0]);
    }
    function returnedStruct() external pure returns (uint256, uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        S memory s = wrap(p); s.a[0] = 9;
        return (p[0], s.a[0]);
    }
    function nestedStruct() external pure returns (uint256, uint256, uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        S memory s = S(p); Pair memory q = Pair(s, s);
        q.left.a[0] = 9;
        return (p[0], s.a[0], q.right.a[0]);
    }
    function assignedMember() external pure returns (uint256, uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        S memory s; s.a = p; s.a[0] = 9;
        return (p[0], s.a[0]);
    }
    function freshConditional(bool flag) external pure returns (uint256, uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        S memory s = flag ? S(p) : S(new uint256[](1)); s.a[0] = 9;
        return (p[0], s.a[0]);
    }
    function inlineArray() external pure returns (uint256, uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        uint256[][2] memory a = [p, p]; a[0][0] = 9;
        return (p[0], a[1][0]);
    }
    function temporaryMember() external pure returns (uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        uint256[] memory q = wrap(p).a; q[0] = 9;
        return p[0];
    }
    function arrayElement() external pure returns (uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        S[] memory a = new S[](1); a[0] = S(p); p[0] = 9;
        return a[0].a[0];
    }
    function identityControl() external pure returns (uint256) {
        uint256[] memory p = new uint256[](1); p[0] = 5;
        uint256[] memory q = identity(p); q[0] = 9;
        return p[0];
    }
}
