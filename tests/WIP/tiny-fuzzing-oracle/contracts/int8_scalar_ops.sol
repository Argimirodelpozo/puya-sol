// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    int8 sc;
    function setSc(int8 v) external { sc = v; }
    function opSc(int8 v) external { unchecked { sc /= v; } }
    function incSc() external { unchecked { sc++; } }
    function decSc() external { unchecked { sc--; } }
    function getSc() external view returns (int8) { return sc; }
}
