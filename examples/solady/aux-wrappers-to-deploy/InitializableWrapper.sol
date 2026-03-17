// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/Initializable.sol";

contract InitializableWrapper is Initializable {
    uint256 public value;

    function initialize(uint256 v) external initializer {
        value = v;
    }

    function getValue() external view returns (uint256) {
        return value;
    }
}
