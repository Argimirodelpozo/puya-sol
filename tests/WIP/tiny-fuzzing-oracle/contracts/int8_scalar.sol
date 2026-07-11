// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    int8 sc;
    function setSc(int8 v) external { sc = v; }
    function divSc(int8 v) external { unchecked { sc /= v; } }
    function incSc() external { unchecked { sc++; } }
    function decSc() external { unchecked { sc--; } }
    function getSc() external view returns (int8) { return sc; }
    function f2() external pure returns (uint256) { return 1; }
}
