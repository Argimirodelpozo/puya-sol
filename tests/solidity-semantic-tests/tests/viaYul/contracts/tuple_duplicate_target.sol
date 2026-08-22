// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// A tuple assignment naming the SAME value-type target more than once, with a
// side-effecting RHS. Solidity evaluates the whole RHS left to right, then
// stores the components right to left, so the surviving value is component 0's.
//
// puyabug.md §13: correct at -O0, wrong at -O2. Dead-store elimination removes
// the overwritten stores but leaves their VALUES on the AVM stack, and the
// return then consumes a leftover instead of reading y.
contract C {
    uint256 x;
    uint256 y;

    function set(uint256 v) public returns (uint256) { x = v; return v; }

    // RHS reads y, so the frontend's snapshot gate fires here.
    function readBack() public returns (uint256, uint256) {
        y = 9;
        (y, y, y) = (set(1), set(2), y);
        return (x, y);   // solc: (2, 1)
    }

    // All-calls RHS: the gate does not fire.
    function allCalls() public returns (uint256, uint256) {
        (y, y, y) = (set(1), set(2), set(3));
        return (x, y);   // solc: (3, 1)
    }
}
