// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract NativePayee {
    constructor() payable {}
    function deposit() external payable {}
}

contract NativeValueKnownApp {
    function pay(NativePayee target, uint256 amount) external {
        target.deposit{value: amount}();
    }

    function create(uint256 amount) external returns (NativePayee) {
        return new NativePayee{value: amount}();
    }
}
