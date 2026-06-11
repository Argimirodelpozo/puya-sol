// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract Child {
    uint256 public seenValue;
    constructor() payable { seenValue = msg.value; }   // reads msg.value -> postInit route
}
contract C {
    Child public child;
    function deploy() external payable returns (uint256 seen, uint256 bal) {
        child = new Child{value: 500000}();
        seen = child.seenValue();
        bal = address(child).balance;
    }
}
