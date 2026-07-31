// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract TupleBool {
    bool[] flags;
    function reset(bool a, bool b, bool c) external { delete flags; flags.push(a); flags.push(b); flags.push(c); }
    function swap01() external { (flags[0], flags[1]) = (flags[1], flags[0]); }
    function pairSet(bool a, bool b) external { (flags[0], flags[1]) = (a, b); }
    function get(uint256 i) external view returns (bool) { return flags[i]; }
    // memory tuple bool assignment
    function memSwap(bool a, bool b, uint256 i) external pure returns (bool, bool) {
        bool[] memory m = new bool[](2); m[0]=a; m[1]=b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    // mixed tuple: (bool, uint)
    function mixed(bool a, uint256 x) external pure returns (bool, uint256) {
        bool[] memory m = new bool[](1); m[0]=false;
        uint256 y;
        (m[0], y) = (a, x);
        return (m[0], y);
    }
}
