// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract BoolPackChar {
    bool[] sba;
    // storage, CONSTANT index write, sibling already set
    function storConstWrite(bool a, bool b) external returns (bool,bool) {
        delete sba; sba.push(a); sba.push(b);
        sba[0] = true;             // const idx 0; sba[1] should be preserved
        return (sba[0], sba[1]);
    }
    // storage, DYNAMIC index write, sibling set
    function storDynWrite(bool a, bool b, uint256 i) external returns (bool,bool) {
        delete sba; sba.push(a); sba.push(b);
        sba[i] = true;             // dynamic idx
        return (sba[0], sba[1]);
    }
    // memory, DYNAMIC index write, sibling set
    function memDynWrite(bool a, bool b, uint256 i) external pure returns (bool,bool) {
        bool[] memory m = new bool[](2); m[0]=a; m[1]=b;
        m[i] = true;
        return (m[0], m[1]);
    }
    // storage bytes-backed? use bool[3] fixed storage, dynamic idx
    bool[3] fba;
    function storFixedDynWrite(bool a, bool b, uint256 i) external returns (bool,bool,bool) {
        fba[0]=a; fba[1]=b; fba[2]=false;
        fba[i] = true;
        return (fba[0], fba[1], fba[2]);
    }
}
