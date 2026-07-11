// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract Cee {
    uint256 public s;
    modifier m() { s += 1; _; }
    function mstup(int256 a) external m returns (int64, uint128) { return (int64(a), uint128(uint256(a))); } // signed tuple + modifier
    function msig(int256 a) external m returns (int64) { return int64(a); }                                   // signed single + modifier
}
contract Cer {
    Cee c;
    constructor() { c = new Cee(); }
    function gmstup(int256 a) external returns (int256) { (int64 x, uint128 y) = c.mstup(a); return int256(x) + int256(uint256(y)); }
    function gmsig(int256 a) external returns (int256) { return int256(c.msig(a)); }
}
