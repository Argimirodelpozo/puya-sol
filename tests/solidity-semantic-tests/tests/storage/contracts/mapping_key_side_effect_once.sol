// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    mapping(uint256 => uint256) m;
    mapping(uint256 => mapping(uint256 => uint256)) n;
    mapping(uint256 => uint256[]) arr;
    function k(uint256 v) internal returns (uint256) { cnt++; return v; }
    function writeOnce() external returns (uint256, uint256) {
        cnt = 0; m[k(1)] = 55; return (m[1], cnt);            // (55, 1)
    }
    function compoundOnce() external returns (uint256, uint256) {
        m[2] = 10; cnt = 0; m[k(2)] += 5; return (m[2], cnt); // (15, 1)
    }
    function readOnce() external returns (uint256, uint256) {
        m[3] = 7; cnt = 0; uint256 v = m[k(3)]; return (v, cnt); // (7, 1)
    }
    function nestedOnce() external returns (uint256, uint256) {
        cnt = 0; n[k(4)][k(5)] = 9; return (n[4][5], cnt);    // (9, 2)
    }
    function deleteOnce() external returns (uint256, uint256) {
        m[6] = 1; cnt = 0; delete m[k(6)]; return (m[6], cnt); // (0, 1)
    }
    function pushOnce() external returns (uint256, uint256) {
        cnt = 0; arr[k(7)].push(42); return (arr[7][0], cnt);  // (42, 1)
    }
}
