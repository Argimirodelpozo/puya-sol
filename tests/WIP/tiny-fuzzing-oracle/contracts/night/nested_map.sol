// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract NestedMap {
    mapping(address => mapping(address => uint256)) public allowance;
    mapping(uint256 => mapping(uint256 => mapping(uint256 => uint256))) public deep;
    struct Acc { uint256 bal; mapping(uint256 => uint256) slots; }
    mapping(address => Acc) accs;
    function approve(address o, address s, uint256 v) external { allowance[o][s] = v; }
    function spend(address o, address s, uint256 v) external { allowance[o][s] -= v; }
    function setDeep(uint256 a, uint256 b, uint256 c, uint256 v) external { deep[a][b][c] = v; }
    function getDeep(uint256 a, uint256 b, uint256 c) external view returns (uint256) { return deep[a][b][c]; }
    function setSlot(address a, uint256 k, uint256 v) external { accs[a].slots[k] = v; accs[a].bal += v; }
    function getSlot(address a, uint256 k) external view returns (uint256,uint256) { return (accs[a].slots[k], accs[a].bal); }
    function clearAllow(address o, address s) external { delete allowance[o][s]; }
}
