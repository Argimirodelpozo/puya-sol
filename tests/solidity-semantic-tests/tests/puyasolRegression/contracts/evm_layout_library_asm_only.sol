// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

library SlotIO {
    function write(uint256 value) internal {
        assembly { sstore(0, value) }
    }

    function read() internal view returns (uint256 value) {
        assembly { value := sload(0) }
    }
}

contract C {
    // Deliberately no declared state: the only storage-runtime references live
    // in a root-level library subroutine.
    function roundtrip(uint256 value) public returns (uint256) {
        SlotIO.write(value);
        return SlotIO.read();
    }
}
