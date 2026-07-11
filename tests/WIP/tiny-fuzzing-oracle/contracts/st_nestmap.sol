// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract NM {
    mapping(uint256 => mapping(uint256 => int128)) public nm;
    mapping(address => uint256) public bal;
    function set(uint256 a, uint256 b, int128 v) external { nm[a][b] = v; }
    function add(uint256 a, uint256 b, int128 d) external { nm[a][b] += d; }   // signed compound, nested mapping value
    function del(uint256 a, uint256 b)           external { delete nm[a][b]; }
    function credit(uint256 who, uint256 amt)    external { bal[address(uint160(who))] += amt; }
}
