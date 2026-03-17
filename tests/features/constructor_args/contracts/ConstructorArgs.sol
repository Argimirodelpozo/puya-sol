// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract ConstructorArgs {
    string public name;
    uint256 public initialValue;
    address public owner;

    constructor(string memory _name, uint256 _initialValue) {
        name = _name;
        initialValue = _initialValue;
        owner = msg.sender;
    }

    function getName() external view returns (string memory) {
        return name;
    }

    function getInitialValue() external view returns (uint256) {
        return initialValue;
    }

    function getOwner() external view returns (address) {
        return owner;
    }
}
