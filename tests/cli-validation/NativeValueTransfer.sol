// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract NativeValueTransfer {
    function pay(address payable recipient) external payable {
        recipient.transfer(msg.value);
    }
}
