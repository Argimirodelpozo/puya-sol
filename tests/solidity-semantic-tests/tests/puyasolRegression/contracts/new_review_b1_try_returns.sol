// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// The inner-call submit/result-capture effects must execute
// before the try-success variables consume the captured returndata.
contract TryReturnsCallee {
    function one(uint256 x) external pure returns (uint256) {
        return x + 7;
    }

    function pair(uint256 x) external pure returns (uint256, uint256) {
        return (x + 1, x + 2);
    }

    function none() external pure {}
}

contract TryReturnsCaller {
    TryReturnsCallee private callee;

    constructor() {
        callee = new TryReturnsCallee();
    }

    function one(uint256 x) external returns (uint256 result) {
        try callee.one(x) returns (uint256 value) {
            result = value + 1;
        } catch {
            result = 999;
        }
    }

    function pair(uint256 x) external returns (uint256 result) {
        try callee.pair(x) returns (uint256 a, uint256 b) {
            result = a * 100 + b;
        } catch {
            result = 999;
        }
    }

    function noReturns() external returns (uint256 result) {
        try callee.none() {
            result = 123;
        } catch {
            result = 999;
        }
    }
}
