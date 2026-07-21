// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards ternary-init storage pointers writing THROUGH for EVERY storage
// family: box-keyed + app-global structs, bytes, fixed arrays, mappings
// (dynamic arrays guarded by ternary_storage_ptr_mutation.sol).
contract TernaryFamilies {
    struct S { uint256 x; uint256 y; }
    mapping(uint256 => S) forceBox; // pushes S into the box-keyed registry
    S s1;
    S s2;
    struct P { uint256 a; } // plain struct, never a mapping value / ref param
    P p1;
    P p2;
    bytes b1;
    bytes b2;
    uint256[3] f1;
    uint256[3] f2;

    function structWrite(bool c, uint256 v) external returns (uint256 r1, uint256 r2) {
        s1.x = 0; s2.x = 0;
        S storage p = c ? s1 : s2;
        p.x = v;
        r1 = s1.x; r2 = s2.x;
    }

    function plainStructWrite(bool c, uint256 v) external returns (uint256 r1, uint256 r2) {
        p1.a = 0; p2.a = 0;
        P storage p = c ? p1 : p2;
        p.a = v;
        r1 = p1.a; r2 = p2.a;
    }

    function bytesPush(bool c) external returns (uint256 l1, uint256 l2) {
        delete b1; delete b2;
        bytes storage p = c ? b1 : b2;
        p.push(0x41);
        l1 = b1.length; l2 = b2.length;
    }

    function fixedWrite(bool c, uint256 v) external returns (uint256 r1, uint256 r2) {
        f1[0] = 0; f2[0] = 0;
        uint256[3] storage p = c ? f1 : f2;
        p[0] = v;
        r1 = f1[0]; r2 = f2[0];
    }
}

contract MappingTernary {
    mapping(uint256 => uint256) m1;
    mapping(uint256 => uint256) m2;
    function mapWrite(bool c, uint256 k, uint256 v) external returns (uint256 r1, uint256 r2) {
        mapping(uint256 => uint256) storage p = c ? m1 : m2;
        p[k] = v;
        r1 = m1[k]; r2 = m2[k];
    }
}
