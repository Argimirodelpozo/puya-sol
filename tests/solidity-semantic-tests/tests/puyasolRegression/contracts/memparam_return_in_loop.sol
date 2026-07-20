// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the write-back augmentation return-walks: a callee mutating a memory
// (or storage-ref library) param with an early `return` inside a LOOP kept the
// return unaugmented — the walk recursed only if/else — so puya rejected valid
// Solidity with a return-type arity mismatch. Both twins (FunctionBuilder for
// contract methods, AWSTBuilder for library/free functions) now share
// forEachReturnStatement (blocks, loops, switch, if/else).
library L {
    function bumpUntilZero(uint256[] memory a) internal pure {
        for (uint256 i = 0; i < a.length; i++) {
            if (a[i] == 0) {
                return; // early return INSIDE the loop
            }
            a[i]++;
        }
    }
}

contract MemParamReturnInLoop {
    function viaLibrary() external pure returns (uint256, uint256, uint256) {
        uint256[] memory a = new uint256[](3);
        a[0] = 10;
        a[1] = 20;
        // a[2] stays 0 → early return on i == 2
        L.bumpUntilZero(a);
        return (a[0], a[1], a[2]);
    }

    function bump(uint256[] memory a) internal pure {
        for (uint256 i = 0; i < a.length; i++) {
            if (a[i] == 0) {
                return;
            }
            a[i] += 5;
        }
    }

    function viaMethod() external pure returns (uint256, uint256) {
        uint256[] memory a = new uint256[](2);
        a[0] = 1;
        a[1] = 2;
        bump(a); // no zero → loop runs to completion, both bumped
        return (a[0], a[1]);
    }
}
