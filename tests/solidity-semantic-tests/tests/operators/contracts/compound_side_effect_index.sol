// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract C {
    // arr[i++] += 5 : EVM evaluates the index i++ ONCE (i:0->1), arr[0]=15.
    // Expected: arr=[15,20,30], i=1. (Regression target: compound-assign must
    // reuse the built LHS rather than rebuild it, which re-ran i++.)
    function memCompound() external pure returns (uint256, uint256, uint256, uint256) {
        uint256[3] memory arr;
        arr[0] = 10; arr[1] = 20; arr[2] = 30;
        uint256 i = 0;
        arr[i++] += 5;
        return (arr[0], arr[1], arr[2], i);
    }
    // Plain-key storage compound (regression guard for the reuse path).
    mapping(uint256 => uint256) bal;
    function stoCompound(uint256 k) external returns (uint256) {
        bal[k] = 100;
        bal[k] += 7;
        return bal[k];   // 107
    }
}
