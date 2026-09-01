// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Storage bool[] — SLOT MODE ONLY. The default storage mode hard-errors on
// this shape (puyabug.md #10: puya's box-backed bool-array append packs one
// BYTE per element while reads getbit — push(true);push(true) came back
// [true, false]). Slot mode uses puya-sol's byte-consistent lowering; the
// raw sload word here is verified against live solc+py-evm.
contract BoolArrayParity {
    bool[] public flags;   // slot 0 — packed 32 bools/slot, one BYTE each

    // The DISCRIMINATING shape: consecutive trues (T,F,T patterns read
    // correctly even under the byte/bit confusion — lucky values).
    function pushReadTrueTrue() public returns (bool ok) {
        flags.push(true);
        flags.push(true);
        ok = flags[0] && flags[1];
    }

    function wordAndOps() public returns (bool ok, uint256 w) {
        // continues from [true, true]
        flags.push(false);
        flags[2] = true;
        flags.pop(); // vacated byte must zero
        bytes32 base = keccak256(abi.encodePacked(uint256(0)));
        assembly { w := sload(base) }
        ok = (w == 0x0101) && (flags.length == 2) && flags[0] && flags[1];
    }
}
