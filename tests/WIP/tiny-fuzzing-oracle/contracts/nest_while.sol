// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // a[i].length in a WHILE condition, no element access
    function condInWhile(uint256[][] calldata a) external pure returns (uint256 s) {
        uint i = 0;
        while (i < a.length) { uint j = 0; while (j < a[i].length) { s += 1; j++; } i++; }
    }
    // full nested sum via while loops
    function sumWhile(uint256[][] calldata a) external pure returns (uint256 s) {
        uint i = 0;
        while (i < a.length) { uint j = 0; while (j < a[i].length) { s += a[i][j]; j++; } i++; }
    }
}
