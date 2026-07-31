// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract StorSwap {
    uint256[] a;      // storage dyn array
    uint256[3] fa;    // storage fixed array
    bool[] b;         // storage bool array
    function setA(uint256 x, uint256 y) external { delete a; a.push(x); a.push(y); }
    function swapA() external { (a[0], a[1]) = (a[1], a[0]); }
    function getA(uint256 i) external view returns (uint256) { return a[i]; }
    function setFa(uint256 x, uint256 y) external { fa[0]=x; fa[1]=y; }
    function swapFa() external { (fa[0], fa[1]) = (fa[1], fa[0]); }
    function getFa(uint256 i) external view returns (uint256) { return fa[i]; }
    function setB(bool x, bool y) external { delete b; b.push(x); b.push(y); }
    function swapB() external { (b[0], b[1]) = (b[1], b[0]); }
    function getB(uint256 i) external view returns (bool) { return b[i]; }
}
