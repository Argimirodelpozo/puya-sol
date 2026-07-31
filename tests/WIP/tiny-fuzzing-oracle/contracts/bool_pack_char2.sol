// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract BoolPackChar2 {
    bool[] sba;
    function storConstWrite(bool a, bool b) external returns (bool,bool) {
        delete sba; sba.push(a); sba.push(b);
        sba[0] = true;
        return (sba[0], sba[1]);
    }
    function storDynWrite(bool a, bool b, uint256 i) external returns (bool,bool) {
        delete sba; sba.push(a); sba.push(b);
        i = i % 2;
        sba[i] = true;
        return (sba[0], sba[1]);
    }
    function memDynWrite(bool a, bool b, uint256 i) external pure returns (bool,bool) {
        i = i % 2;
        bool[] memory m = new bool[](2); m[0]=a; m[1]=b;
        m[i] = true;
        return (m[0], m[1]);
    }
}
