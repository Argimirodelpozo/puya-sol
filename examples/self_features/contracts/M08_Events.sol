// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

contract EventsTest {
    event Transfer(address indexed from, address indexed to, uint256 amount);
    event ValueSet(uint256 newValue);

    uint256 public value;

    function setValue(uint256 newValue) external {
        value = newValue;
        emit ValueSet(newValue);
    }

    function emitTransfer(address to, uint256 amount) external {
        emit Transfer(msg.sender, to, amount);
    }
}
