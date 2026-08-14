// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

contract C {
    modifier touchesSlot() {
        assembly {
            sstore(0, 0x77)
            if iszero(eq(sload(0), 0x77)) { revert(0, 0) }
        }
        _;
    }

    // Deliberately no state and no assembly in the function body itself.
    function run() public touchesSlot returns (uint256) {
        return 1;
    }
}
