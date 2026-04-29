// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M38: Public state variable getters (Gap 9 verification).
 * Tests that `public` state variables automatically generate getter methods.
 */

contract PublicGettersTest {
    uint256 public threshold;
    address public admin;
    bool public paused;
    uint256 public counter;

    constructor() {
        threshold = 100;
        admin = msg.sender;
        paused = false;
        counter = 0;
    }

    function setThreshold(uint256 newThreshold) external {
        threshold = newThreshold;
    }

    function setAdmin(address newAdmin) external {
        admin = newAdmin;
    }

    function setPaused(bool newPaused) external {
        paused = newPaused;
    }

    function increment() external {
        counter += 1;
    }
}
