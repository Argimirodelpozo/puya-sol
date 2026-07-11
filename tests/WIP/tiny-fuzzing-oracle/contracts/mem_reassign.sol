// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract MemReassign {
    function f() external pure returns (uint256) {
        uint256[] memory a = new uint256[](1); a[0] = 5;
        uint256[] memory c = new uint256[](1); c[0] = 9;
        uint256[] memory b = a;   // b aliases a
        b = c;                    // b re-points to c; a MUST stay 5
        return a[0];              // EVM: 5
    }
}
