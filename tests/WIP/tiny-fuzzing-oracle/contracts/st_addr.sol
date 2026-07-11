// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    mapping(address => uint256) public bal;        // address-keyed mapping + getter
    address public owner;                           // address storage + getter
    mapping(address => mapping(address => uint256)) public allow;  // nested address mapping
    function setBal(address a, uint256 v)  external { bal[a] = v; }
    function addBal(address a, uint256 v)  external { bal[a] += v; }
    function setOwner(address a)           external { owner = a; }
    function approve(address o, address s, uint256 v) external { allow[o][s] = v; }
}
