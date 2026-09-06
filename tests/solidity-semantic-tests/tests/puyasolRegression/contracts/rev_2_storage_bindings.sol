// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract BindingLeft {
    uint16 private value = 7;
    uint16[] private items;
    function left() public view returns (uint16, uint256) { return (value, items.length); }
    function setLeft(uint16 v) public { value = v; items.push(v); }
    function clearLeft() public { delete items; }
}

contract BindingRight {
    uint16 private value = 9;
    uint16[] private items;
    function right() public view returns (uint16, uint256) { return (value, items.length); }
    function setRight(uint16 v) public { value = v; items.push(v); }
}

contract BindingLR is BindingLeft, BindingRight {}
contract BindingRL is BindingRight, BindingLeft {}
