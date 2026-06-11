// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract Callee {
    function take(bytes memory b) external pure returns (uint256) { return b.length; }
}
contract C {
    uint256 public cnt;
    Callee callee;
    constructor() { callee = new Callee(); }
    function mkBytes() internal returns (bytes memory) { cnt++; return hex"aabbcc"; }
    function extOnce() external returns (uint256, uint256) {
        cnt = 0;
        uint256 len = callee.take(mkBytes());
        return (len, cnt);   // expect (3, 1)
    }
}
