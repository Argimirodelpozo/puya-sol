// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract BoolArrRW {
    function rt(uint256 i, bool v) external pure returns (bool) {
        bool[] memory f = new bool[](8);
        f[i] = v;
        return f[i];
    }
    function multi(bool a, bool b, bool c, uint256 i) external pure returns (bool) {
        bool[] memory f = new bool[](3);
        f[0] = a; f[1] = b; f[2] = c;
        return f[i];
    }
    function toggle(uint256 i, bool first) external pure returns (bool) {
        bool[] memory f = new bool[](4);
        f[i] = first;
        f[i] = !first;
        return f[i];
    }
    function copyCount(bool[] calldata src) external pure returns (uint256 n) {
        bool[] memory f = new bool[](src.length);
        for (uint256 k = 0; k < src.length; k++) f[k] = src[k];
        for (uint256 k = 0; k < f.length; k++) if (f[k]) n++;
    }
}
