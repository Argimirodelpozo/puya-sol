// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    function a() internal returns (int16) { cnt++; return 20; }
    function b() internal returns (int16) { cnt++; return 6; }
    function sdiv() external returns (int256, uint256) { cnt = 0; int16 r = a() / b(); return (int256(r), cnt); } // 3
    function smod() external returns (int256, uint256) { cnt = 0; int16 r = a() % b(); return (int256(r), cnt); } // 2
    function sexp() external returns (int256, uint256) { cnt = 0; int16 r = b() ** 2;  return (int256(r), cnt); } // 36, base once
}
