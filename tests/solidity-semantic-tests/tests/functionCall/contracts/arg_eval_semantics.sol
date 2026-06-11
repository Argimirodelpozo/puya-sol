// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    function a() internal returns (uint256) { cnt++; return 10; }
    function b() internal returns (uint256) { cnt++; return 20; }
    function add2(uint256 x, uint256 y) internal pure returns (uint256) { return x + y; }
    // each arg call once -> cnt==2, result 30
    function argOnce() external returns (uint256, uint256) { cnt = 0; uint256 r = add2(a(), b()); return (r, cnt); }
    // eval order left-to-right: record order via cnt-encoding
    uint256 public seq;
    function la() internal returns (uint256) { seq = seq * 10 + 1; return 0; }
    function lb() internal returns (uint256) { seq = seq * 10 + 2; return 0; }
    function order() external returns (uint256) { seq = 0; add2(la(), lb()); return seq; }   // expect 12
    // nested arg call once
    function id(uint256 x) internal pure returns (uint256) { return x; }
    function nested() external returns (uint256, uint256) { cnt = 0; uint256 r = id(a()); return (r, cnt); } // r=10 cnt=1
    // external (this.) call arg eval once
    function ext(uint256 x, uint256 y) external pure returns (uint256) { return x + y; }
    function viaThis() external returns (uint256, uint256) { cnt = 0; uint256 r = this.ext(a(), b()); return (r, cnt); } // r=30 cnt=2
}
