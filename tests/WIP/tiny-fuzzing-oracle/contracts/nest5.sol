// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // a[i].length IN the inner-loop condition, NO a[i][j] in the body
    function condInLoop(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++)
            for (uint j; j < a[i].length; j++) s += 1;
    }
    // a[i].length cached to a local BEFORE the inner loop; condition uses the cache
    function condCached(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) {
            uint len = a[i].length;
            for (uint j; j < len; j++) s += 1;
        }
    }
}
