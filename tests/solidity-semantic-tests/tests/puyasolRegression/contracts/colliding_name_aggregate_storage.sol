// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Colliding AGGREGATE declarations across bases: writers, element/length
// readers, and the auto-getter's hash chain must all key storage by the same
// disambiguated physical binding, or one base's writes are invisible to its
// own reads (the box-key split-brain family).
contract ArrayBase {
    uint256[] private data;
    bytes private blob;

    function pushA(uint256 v) public { data.push(v); }
    function lenA() public view returns (uint256) { return data.length; }
    function atA(uint256 i) public view returns (uint256) { return data[i]; }
    function setBlobA(bytes calldata b) public { blob = b; }
    function blobLenA() public view returns (uint256) { return blob.length; }
}

contract MapBase {
    uint256[] private data;
    bytes private blob;
    mapping(address => uint256) public bal;

    function pushB(uint256 v) public { data.push(v); }
    function lenB() public view returns (uint256) { return data.length; }
    function atB(uint256 i) public view returns (uint256) { return data[i]; }
    function setBal(address a, uint256 v) public { bal[a] = v; }
    function readBal(address a) public view returns (uint256) { return bal[a]; }
}

contract SiblingMap {
    mapping(address => uint256) private bal;

    function setOther(address a, uint256 v) public { bal[a] = v; }
    function readOther(address a) public view returns (uint256) { return bal[a]; }
}

contract C is ArrayBase, MapBase, SiblingMap {}
