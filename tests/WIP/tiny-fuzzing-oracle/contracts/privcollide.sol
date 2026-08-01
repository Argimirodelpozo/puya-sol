// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Two DISTINCT private state vars with the SAME name in different base
// contracts. Solidity allows this (private => no shadowing across bases);
// they occupy separate storage slots.
contract Base1 {
    string private _v;
    constructor(string memory s) { _v = s; }
    function fromBase1() external view returns (string memory) { return _v; }
}

contract Base2 {
    bytes32 private _v;
    constructor(bytes32 b) { _v = b; }
    function fromBase2() external view returns (bytes32) { return _v; }
}

contract Both is Base1, Base2 {
    constructor() Base1("hello") Base2(bytes32(uint256(0x2a))) {}
}
