// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract MemTupleStress {
    function rotate3(uint256 a, uint256 b, uint256 c) external pure returns (uint256,uint256,uint256) {
        uint256[] memory m = new uint256[](3); m[0]=a; m[1]=b; m[2]=c;
        (m[0], m[1], m[2]) = (m[2], m[0], m[1]);
        return (m[0], m[1], m[2]);
    }
    function dynIdx(uint256 a, uint256 b, uint256 i, uint256 j) external pure returns (uint256,uint256) {
        i = i % 4; j = j % 4;
        uint256[] memory m = new uint256[](4); m[0]=a; m[1]=b; m[2]=a+1; m[3]=b+1;
        (m[i], m[j]) = (m[j], m[i]);
        return (m[i], m[j]);
    }
    function mixedLocal(uint256 a, uint256 x) external pure returns (uint256,uint256) {
        uint256[] memory m = new uint256[](1); m[0]=a;
        (m[0], x) = (x, m[0]);
        return (m[0], x);
    }
    function nested(uint256 a, uint256 b) external pure returns (uint256,uint256) {
        uint256[][] memory mm = new uint256[][](1);
        mm[0] = new uint256[](2); mm[0][0]=a; mm[0][1]=b;
        (mm[0][0], mm[0][1]) = (mm[0][1], mm[0][0]);
        return (mm[0][0], mm[0][1]);
    }
    function boolRot(bool a, bool b, bool c) external pure returns (bool,bool,bool) {
        bool[] memory m = new bool[](3); m[0]=a; m[1]=b; m[2]=c;
        (m[0], m[1], m[2]) = (m[2], m[0], m[1]);
        return (m[0], m[1], m[2]);
    }
}
