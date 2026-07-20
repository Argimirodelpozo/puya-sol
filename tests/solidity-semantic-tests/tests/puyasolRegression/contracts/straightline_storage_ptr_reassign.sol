// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM companion to cond_storage_ptr_reassign.sol: the SOUND forms must keep
// compiling and behaving — straight-line reassignment and ternary selection at
// initialization (the RHS conditional is a runtime expression; only the
// ASSIGNMENT itself being conditionally executed is unsupported).
contract StraightlineStoragePtrReassign {
    uint256[] a1;
    uint256[] a2;

    function straight() external returns (uint256 l1, uint256 l2) {
        uint256[] storage p = a1;
        p.push(1);
        p = a2;
        p.push(2);
        p.push(3);
        l1 = a1.length;
        l2 = a2.length;
    }

    // READ through a ternary-selected pointer. (Known gap, out of scope
    // here: MUTATING through a ternary-init pointer pushes into a value
    // copy — see fable-review-3.)
    function ternaryLen(bool c) external view returns (uint256) {
        uint256[] storage p = c ? a1 : a2;
        return p.length;
    }
}
