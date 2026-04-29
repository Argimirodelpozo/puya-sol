// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/ReentrancyGuard.sol";

contract ReentrancyGuardWrapper is ReentrancyGuard {
    uint256 public counter;

    function increment() external nonReentrant {
        counter += 1;
    }

    function getCounter() external view returns (uint256) {
        return counter;
    }
}
