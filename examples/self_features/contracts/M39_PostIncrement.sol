// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M39: Pre/post increment/decrement correctness (Gap 10 verification).
 * Tests that i++ returns old value, ++i returns new value.
 */

contract PostIncrementTest {
    mapping(address => uint256) public nonces;
    uint256 public counter;

    constructor() {
        counter = 0;
    }

    /// Post-increment on local variable: returns old value
    function testLocalPostInc(uint256 start) external pure returns (uint256 result, uint256 after_) {
        uint256 x = start;
        result = x++;
        after_ = x;
    }

    /// Pre-increment on local variable: returns new value
    function testLocalPreInc(uint256 start) external pure returns (uint256 result, uint256 after_) {
        uint256 x = start;
        result = ++x;
        after_ = x;
    }

    /// Post-decrement on local variable: returns old value
    function testLocalPostDec(uint256 start) external pure returns (uint256 result, uint256 after_) {
        uint256 x = start;
        result = x--;
        after_ = x;
    }

    /// Pre-decrement on local variable: returns new value
    function testLocalPreDec(uint256 start) external pure returns (uint256 result, uint256 after_) {
        uint256 x = start;
        result = --x;
        after_ = x;
    }

    /// Post-increment on mapping: nonces[sender]++ returns old nonce
    function useNonce(address owner) external returns (uint256 oldNonce) {
        oldNonce = nonces[owner]++;
    }

    /// Read nonce value (getter is auto-generated but we add explicit one too)
    function getNonce(address owner) external view returns (uint256) {
        return nonces[owner];
    }

    /// Post-increment on state variable: counter++ returns old value
    function postIncCounter() external returns (uint256 oldVal) {
        oldVal = counter++;
    }

    /// Pre-increment on state variable: ++counter returns new value
    function preIncCounter() external returns (uint256 newVal) {
        newVal = ++counter;
    }

    /// Standalone post-increment (result discarded, just side effect)
    function incrementNonce(address owner) external {
        nonces[owner]++;
    }
}
