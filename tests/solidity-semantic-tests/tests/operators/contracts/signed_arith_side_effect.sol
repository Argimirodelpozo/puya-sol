// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    function a() internal returns (int8) { cnt++; return 3; }
    function b() internal returns (int8) { cnt++; return 4; }
    // Each operand call must execute exactly ONCE -> cnt==2 in every case.
    function sadd() external returns (int256, uint256) { cnt = 0; int8 r = a() + b(); return (int256(r), cnt); }
    function ssub() external returns (int256, uint256) { cnt = 0; int8 r = b() - a(); return (int256(r), cnt); }
    function smul() external returns (int256, uint256) { cnt = 0; int8 r = a() * b(); return (int256(r), cnt); }
}
