// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function eq(address a, address b)  external pure returns (bool) { return a == b; }
    function neq(address a, address b) external pure returns (bool) { return a != b; }
    function isZero(address a)         external pure returns (bool) { return a == address(0); }
    function pick(address a, address b, bool c) external pure returns (address) { return c ? a : b; }   // address return
    function pair(address a, address b) external pure returns (address, address) { return (b, a); }     // multi addr return
    mapping(address => uint256) bal;
    function setBal(address a, uint256 v) external { bal[a] = v; }
    function getBal(address a) external view returns (uint256) { return bal[a]; }
}
