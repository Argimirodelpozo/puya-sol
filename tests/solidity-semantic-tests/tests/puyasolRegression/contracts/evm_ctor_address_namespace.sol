// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract CtorMint {
    mapping(address => uint256) public bal;

    constructor(address seed) {
        bal[seed] = 1000;
    }

    function spend(uint256 n) external returns (uint256) {
        require(bal[msg.sender] >= n, "insufficient");
        bal[msg.sender] -= n;
        return bal[msg.sender];
    }
}
