// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract DataLoc {
    struct S { uint256 x; uint256 y; }
    S sStruct;
    uint256[] sArr;
    S[] sStructs;
    mapping(uint256 => S) sMap;

    // memory copy of storage struct: mutation must NOT write back
    function memCopyStruct(uint256 a, uint256 b, uint256 nv) external returns (uint256, uint256) {
        sStruct = S(a, b);
        S memory m = sStruct;      // COPY
        m.x = nv;                  // must not affect sStruct
        return (sStruct.x, m.x);   // expect (a, nv)
    }
    // storage reference: mutation MUST write back
    function storRefStruct(uint256 a, uint256 b, uint256 nv) external returns (uint256) {
        sStruct = S(a, b);
        S storage s = sStruct;     // ALIAS
        s.x = nv;
        return sStruct.x;          // expect nv
    }
    // memory copy of storage array: mutation must NOT write back
    function memCopyArr(uint256 a, uint256 b, uint256 nv) external returns (uint256, uint256) {
        delete sArr; sArr.push(a); sArr.push(b);
        uint256[] memory m = sArr; // COPY
        m[0] = nv;
        return (sArr[0], m[0]);    // expect (a, nv)
    }
    // storage ref to array-of-struct element: mutation MUST write back
    function storRefElem(uint256 a, uint256 b, uint256 nv) external returns (uint256) {
        delete sStructs; sStructs.push(S(a, b));
        S storage s = sStructs[0];
        s.y = nv;
        return sStructs[0].y;      // expect nv
    }
    // storage ref to mapping value struct
    function storRefMap(uint256 k, uint256 a, uint256 nv) external returns (uint256) {
        sMap[k] = S(a, a);
        S storage s = sMap[k];
        s.x = nv;
        return sMap[k].x;          // expect nv
    }
    // function arg passed by memory (copy) -> no writeback
    function _bump(S memory m) internal pure { m.x += 1000; }
    function memArgCopy(uint256 a) external returns (uint256) {
        sStruct = S(a, a);
        _bump(sStruct);            // passes a COPY -> no writeback
        return sStruct.x;          // expect a
    }
}
