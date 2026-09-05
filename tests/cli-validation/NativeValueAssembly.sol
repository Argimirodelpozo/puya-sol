// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract NativeValueAssembly {
    function pay(address recipient, uint256 amount) external returns (uint256 ok) {
        assembly { ok := call(gas(), recipient, amount, 0, 0, 0, 0) }
    }

    function payWord(uint256 recipient, uint256 amount) external returns (uint256 ok) {
        assembly { ok := call(gas(), recipient, amount, 0, 0, 0, 0) }
    }
}
