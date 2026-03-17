// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract Modifiers {
    address public owner;
    bool public paused;
    uint256 public value;

    constructor() {
        owner = msg.sender;
    }

    modifier onlyOwner() {
        require(msg.sender == owner, "not owner");
        _;
    }

    modifier whenNotPaused() {
        require(!paused, "paused");
        _;
    }

    // Modifier with epilog (like Uniswap V2 lock)
    uint256 private unlocked = 1;
    modifier lock() {
        require(unlocked == 1, "locked");
        unlocked = 0;
        _;
        unlocked = 1;
    }

    function setValue(uint256 v) external onlyOwner whenNotPaused {
        value = v;
    }

    function pause() external onlyOwner {
        paused = true;
    }

    function unpause() external onlyOwner {
        paused = false;
    }

    function lockedIncrement() external lock returns (uint256) {
        value += 1;
        return value;
    }

    function getUnlocked() external view returns (uint256) {
        return unlocked;
    }
}
