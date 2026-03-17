// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/Multicallable.sol";

contract MulticallableWrapper is Multicallable {
    uint256 public value;

    function setValue(uint256 v) external {
        value = v;
    }

    function getValue() external view returns (uint256) {
        return value;
    }
}
