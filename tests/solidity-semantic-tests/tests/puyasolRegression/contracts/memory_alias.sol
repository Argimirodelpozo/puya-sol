// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test.
// Memory→memory assignment ALIASES (EVM) via copy-elision; reassignment falls back to copy.
contract MemoryAlias {
    function aliases() external pure returns (uint256) {
        uint256[] memory a = new uint256[](1); a[0] = 5;
        uint256[] memory b = a;   // b aliases a
        b[0] = 11;                // mutates the shared array
        return a[0];              // EVM: 11
    }
    function reassignIsSafe() external pure returns (uint256) {
        uint256[] memory a = new uint256[](1); a[0] = 5;
        uint256[] memory c = new uint256[](1); c[0] = 9;
        uint256[] memory b = a;   // would-alias, but b is reassigned below → falls back to copy
        b = c;                    // b re-points to c; a must be unchanged
        return a[0];              // EVM: 5
    }
}
