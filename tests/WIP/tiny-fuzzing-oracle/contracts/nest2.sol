// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // variable outer index in a loop, .length only (no element access)
    function loopLen(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) s += a[i].length;
    }
    // constant double-index element access (no loop, no variable index)
    function constElem(uint256[][] calldata a) external pure returns (uint256) {
        if (a.length > 0 && a[0].length > 0) return a[0][0];
        return 0;
    }
    // variable outer index, constant inner element
    function varOuter(uint256[][] calldata a) external pure returns (uint256) {
        if (a.length > 0 && a[0].length > 0) { uint i = 0; return a[i][0]; }
        return 0;
    }
}
